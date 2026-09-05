# Rashid — Rosetta 亡き後も Intel アプリを動かす

[English README](README.md)

## 目標

**Rosetta が macOS から削除された後も、Apple Silicon 上で Intel (x86_64) の
macOS アプリを起動できること。**

達成基準:

1. Rosetta が存在しない macOS で、x86_64 の Mach-O を rashid が起動できる
2. Apple のプライベート entitlement / SIP 保護領域 / カーネルの翻訳経路に
   一切依存しない (= 通常ユーザ権限で動く)
3. 再配布可能 — Apple のコードを同梱しない

Rosetta 本体は流用できない。実測で確認済み (「Rosetta を流用できない理由」
参照): 入口 (カーネルの exec 判定) と出口 (libRosettaRuntime) の両方が
Apple 側にあり、真ん中の翻訳器だけ取り出しても繋がらない。
だから翻訳レイヤは自作する。

Wine が Win32 API を再実装するのとは違い、**macOS のフレームワークは
arm64 版が既に存在する**ため、API の再実装は不要。必要なのは
CPU 命令の翻訳と、ネイティブ dylib を呼ぶための ABI 変換 (thunk) だけ。

## アーカイブの役割

`rosetta-archive-<version>/` は目標に対して**ビルド時の依存**であり、
実行時の依存ではない。thunk 方式では実行時に呼ぶのはネイティブ arm64 の
フレームワークなので、x86_64 の dylib は成果物に同梱しない (達成基準 3)。

| 退避物 | 用途 | 時点 |
|---|---|---|
| x86_64 dyld 共有キャッシュ | thunk シグネチャの生成元 (型エンコーディング) | ビルド時 |
| MacOSX.sdk | x86_64 テストバイナリのコンパイル | ビルド時 |
| Rosetta ランタイム | 翻訳品質の参照・検証 | 開発時のみ |
| AOT キャッシュ | 同上 | 開発時のみ |

例外は将来のフォールバック経路。thunk を書いていないマイナーな
PrivateFramework については、その x86_64 版を丸ごと翻訳して動かす方が
安上がりになる場合がある。その経路を採るなら共有キャッシュは実行時依存に
変わり、達成基準 3 と衝突するので、ユーザが自分の環境から用意する形になる。

## 現状 (M1a まで)

動くもの:

- x86_64 Mach-O のロード (thin / fat、LC_MAIN / LC_UNIXTHREAD の両方)
- **独立したゲストアドレス空間** — ゲストは希望する vmaddr をそのまま使える
  (slide なし)。全メモリアクセスを境界・権限チェックするので、暴走した
  ゲストポインタは rashid に届かず、その場で fault として報告される
- 整数命令サブセットのインタプリタ (ALU, flags, jcc, call/ret, mul/div,
  movzx/movsx, cmovcc, setcc, shld/shrd)
- macOS BSD syscall の一部 (`read` / `write` / `close` / `exit`)

動かないもの: dylib を import するバイナリ全部 (= 実アプリ全部)。
SSE/AVX、x87、TLS (`%fs`/`%gs` セグメント)、シグナル、スレッド、
ゲストからの `mmap`。

```
make             # ビルド
make test        # freestanding ゲスト 2 本 + fault 封じ込め 3 種
./rashid -l FILE    # ロードだけして構造をダンプ
./rashid -m FILE    # ゲストアドレス空間マップを表示
./rashid -t FILE    # 全命令トレース
```

### ゲストアドレス空間

ゲストアドレスはホストアドレスではなく、1 個の大きな予約領域へのオフセット。

```
ホスト空間                        ゲスト空間 (8GiB 予約)
  0x100000000  rashid 自身の __TEXT    0x100000000  ゲストの __TEXT
  0xa58000000  ゲスト空間の実体 ──→ 0x000000000  ゲストの 0
               (ASLR で毎回変わる。ゲストからは観測不能)
```

両方が `0x100000000` を使えるのはこのため。JIT (M5) ではベースを
レジスタ 1 本に固定する — FEX や box64 と同じ方式。

## アーキテクチャ

```
   x86_64 アプリ本体
        │  ← ここだけを翻訳する
   ┌────┴─────────────────────────────┐
   │ rashid translator (JIT)             │
   │  decode → IR → AArch64 emit      │
   └────┬─────────────────────────────┘
        │  dylib 呼び出しを検出
   ┌────┴─────────────────────────────┐
   │ thunk layer  (SysV → AAPCS64)    │
   │  ・ObjC: method_getTypeEncoding  │
   │         から実行時に自動生成      │
   │  ・C   : .tbd + SDK ヘッダから    │
   │         libclang で事前生成       │
   └────┬─────────────────────────────┘
        │
   ネイティブ arm64 の AppKit / Metal / libSystem  ← 翻訳しない
```

要点は **ObjC ランタイムが型情報を自分で持っている**こと。
Cocoa の呼び出しはほぼ全て `objc_msgSend` を通り、
`method_getTypeEncoding()` が完全なシグネチャを返すので、
Cocoa 面の thunk は実行時に自動生成できる。手書きが要るのは C API だけ。

## tools/ — 共有キャッシュ読み取り

Apple の `dsc_extractor.bundle` はキャッシュを個別 Mach-O に展開できるが、
**セレクタ名を復元できない**。現行キャッシュではメソッドのセレクタが
キャッシュ全体で共有された 1 個のセレクタプールへのオフセットになっており、
そのプールはどの単体 dylib にも含まれないため。`dsc.py` はキャッシュを
直接読むのでセレクタ・型エンコーディング・エクスポートが全て解決する。

```
./tools/dsc.py info    <cache>            ヘッダ・mapping・セレクタプール
./tools/dsc.py list    <cache> [pat]      収録イメージ一覧
./tools/dsc.py objc    <cache> <pat>      クラス・メソッド・型エンコーディング
./tools/dsc.py symbols <cache> <pat>      エクスポートシンボル
./tools/dsc.py plan    <cache> [pat]      thunk 作業量の集計
./tools/objc_live      -c <Class>         ライブ arm64 ランタイム (検証用)
```

解読した点:

- キャッシュ内ポインタは slide 符号化のまま。
  `value = (raw & 0xFF0000FFFFFFFFFF) + 0x7FF800000000`
- 小メソッド (`entsize=12`) で `0x40000000` が立つと、セレクタは
  フィールド相対ではなく**セレクタプール基準からのオフセット**。
  プール先頭は objc のセンチネル `"🤯\0"`。ただしこの文字列は通常の
  文字列セクションにも出現する (このキャッシュでは 4 箇所) ため、
  実際のメソッドリストのオフセットで自己校正して基準を確定する。
- 検証: `NSProgressPublisherProxy` の 18 メソッドがライブ arm64 ランタイムと
  セレクタ集合で完全一致。

### x86_64 と arm64 で型エンコーディングは一致しない

| セレクタ | x86_64 | arm64 |
|---|---|---|
| `isFromConnection:` | `c24@0:8@16` | `B24@0:8@16` |

`BOOL` が x86_64 では `signed char` (`c`)、arm64 では `bool` (`B`)。
**ライブ arm64 ランタイムを thunk 生成元に代用することはできない。**
x86_64 キャッシュのアーカイブが必須である根拠。

### M4 の作業量 (実測)

| | クラス | メソッド | 型情報あり | C 関数 |
|---|---|---|---|---|
| AppKit | 2573 | 44061 | 44051 (99.98%) | 7207 |
| 主要6フレームワーク | 4243 | 65693 | 65678 (99.98%) | 32444 |

ObjC 面は事実上 100% 自動生成できる。手書きが要るのは C 関数側だけで、
かつ実アプリが実際に呼ぶのはこのうちごく一部。

## テスト

ゲストは freestanding な x86_64 バイナリで、計算結果を終了コードとして返す。
`make test` はその値を2通りで検証する — 実機の x86_64 から記録した値と、
**このマシンに Rosetta がまだある間は**同じバイナリを実機で走らせた結果と。

実機比較のほうが強い。自分の思い込みではなく本物の CPU と突き合わせるため。
そして macOS 28 で失われる。だから今のうちに期待値を記録しておく。

既に効果があった。`tests/ripimm.x86` は、RIP 相対変位と即値が同時に現れる
命令で宛先が 4 バイト手前になるバグの回帰テスト。変位は即値を含む
**命令全体**の末尾が基準であるため。誤った宛先はどれも「近くのそれらしい
アドレス」だったので、fault にもならず静かに壊れていた。

## ロードマップ

| | 内容 | 状態 |
|---|---|---|
| M0 | Mach-O ローダ + 整数インタプリタ | **完了** |
| T0 | 共有キャッシュ読み取り (`tools/dsc.py`) | **完了** |
| M1a | ゲストアドレス空間の分離 | **完了** |
| M1b | TLS (`%gs`) 完了。mmap とシグナルが残り | 一部 |
| M2 | SSE2 + x87 (80bit はソフト実装) | |
| M3 | dyld 相当: 依存解決、chained fixups、stub の thunk 化 | |
| M4 | `objc_msgSend` thunk の自動生成 → 最初の Cocoa アプリ起動 | |
| M5 | basic-block JIT (MAP_JIT + `pthread_jit_write_protect_np`) | |
| M6 | AOT キャッシュ、trace JIT | |

**目標到達は M4**。そこで最初の Cocoa アプリが起動する。M1〜M3 は土台、
M5/M6 は速度改善であって、起動可否には効かない。

## 既知の設計課題

**~~ゲストアドレス空間の衝突~~ (M1a で解決)**
rashid 自身が `0x100000000` にロードされ x86_64 の希望ベースと衝突していた。
リンカでの回避は不可能だった (arm64 では `-no_pie` も `-image_base` も
無視され、`-pagezero_size` は 4GB で頭打ち)。ソフトウェア MMU にして
アドレス空間ごと分離することで解決。副産物としてゲストの隔離も得られた。

**ページサイズ**
x86_64 Mach-O は 4KB 境界、Apple Silicon は 16KB ページ。セグメントを
個別に file-map できないため、匿名マップへ copy-in している
(`macho.c` の pass 2)。権限はゲストの 4KB 粒度で `as->perm` に持ち、
ホストの mprotect は 16KB に丸める。したがって 16KB 内で権限が異なる
ケースはホスト MMU では守られないが、`rsd_as_ok()` が防ぐ。

**TSO (最大の壁)**
x86 は強いメモリ順序、ARM は弱い順序。M1 は TSO モードを
ハードウェアで持つが Apple は Rosetta 専用にしか有効化しないため、
第三者は全メモリアクセスを `ldar`/`stlr` にするしかない。
マルチスレッドアプリで大きな性能ペナルティになる。

**W^X**
JIT には `MAP_JIT` + `pthread_jit_write_protect_np()` +
`com.apple.security.cs.allow-jit` entitlement + Hardened Runtime が必要。
RW/RX はスレッド単位で切り替わるので、翻訳スレッドの設計に影響する。

## Rosetta を流用できない理由 (実測)

macOS 26.6.2 で確認:

| | 実測結果 |
|---|---|
| 入口 | x86_64 の exec 判定はカーネル内。`sysctl.proc_translated` が返す |
| entitlement | `oahd` は `com.apple.private.oahd` 等を要求。署名は "macOS Software Signing"。AMFI がカーネルで検証するため自己署名不可 |
| 設置場所 | `/usr/libexec/rosetta/`、`/System/Library/LaunchDaemons/`、`/var/db/oah` (listing すら Operation not permitted) — すべて SIP 配下 |
| ランタイム | `libRosettaRuntime` は dylib ではなく `MH_EXECUTE`。エクスポートは 2 個のみ。dlopen して翻訳 API を呼ぶことはできない |

流用できたもの: `dsc_extractor.bundle` のみ (キャッシュを個別 Mach-O に展開できる)。
`translate_tool` は単体では出力を出さず、仮に出せても AOT 成果物は
libRosettaRuntime が読む private 形式なので、実行側なしでは動かない。

いずれにせよ Rashid は Rosetta を使わない。上は「近道が無い」ことの記録であって、
Apple バイナリの改変を勧めるものではない。

## 参考実装

- **box64** — ネイティブライブラリ wrapping の実装パターン。方式が同じ
- **FEX-Emu** — x86_64→AArch64 JIT の品質と TSO 対策
- **blink** — 小さい x86-64 エミュレータ。最初に読むならこれ
- **[Darling](https://github.com/darlinghq/darling)** — 逆方向 (Linux 上で macOS
  アプリ)。AppKit を再実装する必要があるため長年 GUI アプリがほぼ動かない。
  Rashid がその再実装を必要としない理由の対照例として有用
- **[rozetka2](https://github.com/XS-Corp/rozetka2)** — 最も近い兄弟プロジェクト。
  同じく clean-room の x86_64→arm64 translator、MIT。MIT のコードは
  そのまま取り込める。逆方向は Apache-2.0 の条項が付いて回る

## ライセンス

Apache-2.0。[LICENSE](LICENSE) と [NOTICE](NOTICE) を参照。

寛容なライセンスかつ特許条項付き。命令翻訳は特許が密集する領域なので、
この点が実質的に効く。

「Rosetta」「macOS」「Apple Silicon」は Apple Inc. の商標であり、
Rashid は Apple Inc. と提携も承認もされていない。
