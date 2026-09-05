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

## 現状 (M2 まで)

動くもの:

- x86_64 Mach-O のロード (thin / fat、LC_MAIN / LC_UNIXTHREAD の両方)
- 共有アドレス空間と領域ごとの権限追跡。暴走したゲストポインタは
  翻訳器を巻き込まず、その場で正確に報告される
- 整数命令サブセット (キャリー付き `adc`/`sbb` を含む ALU、flags、jcc、
  call/ret、mul/div、movzx/movsx、cmovcc、setcc、shld/shrd)
- **TLS**。macOS x86_64 はスレッドの TSD を `%gs` に置き、machine-dependent
  syscall `0x3000003` で設定する。`pthread_getspecific` は
  `movq %gs:(,%rdi,8), %rax` の1命令 (`%fs` は macOS では未使用)
- **SSE**。コンパイラ生成コードが実行する SIMD 命令の 99.3% をカバー (下記)
- ゲストのメモリ管理: `mmap` (匿名)、`mprotect`、`munmap`
- クラス判定付きの syscall: `read` / `write` / `close` / `exit` /
  `thread_fast_set_cthread_self`

動かないもの: dylib を import するバイナリ全部 (= 実アプリ全部)。
AVX、x87、シグナル、スレッド、ファイル実体を伴う `mmap`。

```
make             # ビルド
make test        # freestanding ゲスト 2 本 + fault 封じ込め 3 種
./rashid -l FILE    # ロードだけして構造をダンプ
./rashid -m FILE    # ゲストアドレス空間マップを表示
./rashid -t FILE    # 全命令トレース
```

### アドレス空間

**ゲストは rashid とアドレス空間を共有する。ゲストポインタはホストポインタ
そのものである。**

これは選択ではなく強制である。翻訳されたコードがネイティブ arm64 の
フレームワークを呼ぶときポインタを渡し、ネイティブ側はゲストが後で読む
メモリにポインタを書き込む — 構造体の中、ObjC オブジェクトの中、
コールバックに渡されるバッファの中に。境界で変換するにはポインタのグラフを
全て辿ることになり非現実的なので、両者はアドレスの意味について合意して
いなければならない。Wine も box64 も同じ理由でアドレス空間を共有している。

帰結として、イメージは必ずしもリンクされたアドレスに置けない。
x86_64 イメージは `0x100000000` を望むが、そこは arm64 実行ファイル
すなわち rashid 自身が置かれる場所であり、リンカは動かしてくれない
(arm64 では `-no_pie` も `-image_base` も無視され、`-pagezero_size` は
4GB で頭打ち)。したがって PIE イメージは slide し、非 PIE イメージは
黙って誤配置せず明確なメッセージで拒否する。2011 年頃以降にビルドされた
x86_64 macOS アプリは全て PIE である。

rashid が追跡するのは、ゲストに渡した領域とその権限。全アクセスをそれと
照合するので、暴走したポインタは「どこか後で落ちる」ではなくその場の
正確な報告になる。サンドボックスではなくデバッグ支援であり、thunk が
入れば緩める必要がある — その時点でゲストは正当にネイティブメモリへ
到達するようになるため。

権限は領域ごとに記録し `mprotect` ではなくソフトウェアで強制する。
ホストページは 16KB、x86_64 イメージの配置は 4KB 境界なので、1つの
ホストページが権限の異なるセグメントにまたがることが常態化している。
権限を外側に丸めると隣のセグメントが黙って書き込み不可になる。

## テスト

ゲストは freestanding な x86_64 バイナリで、計算結果を終了コードとして返す。
`make test` はその値を2通りで検証する — 実機の x86_64 から記録した値と、
**このマシンに Rosetta がまだある間は**同じバイナリを実機で走らせた結果と。

```
== behaviour (exit status must match real x86_64) ==
  hello    rashid=0    native=0    ok
  arith    rashid=55   native=55   ok
  tls      rashid=15   native=15   ok
  ripimm   rashid=255  native=255  ok
  sse      rashid=21   native=21   ok
  vm       rashid=9    native=9    ok
```

実機比較のほうが強い。自分の思い込みではなく本物の CPU と突き合わせるため。
そして macOS 28 で失われる。だから今のうちに期待値を記録しておく。

既に3回効果があった。RIP 相対変位が即値の分だけずれるバグ、`0F C2`
(`cmpeqsd` — C の `==` はこれになる) の未実装、`adc`/`sbb` の未実装。
どれも「近くのそれらしい値」を返すため、実機比較なしでは気づきにくい。

## ロードマップ

| | 内容 | 状態 |
|---|---|---|
| M0 | Mach-O ローダ + 整数インタプリタ | **完了** |
| T0 | 共有キャッシュ読み取り (`tools/dsc.py`) | **完了** |
| M1a | アドレス空間と権限追跡 | **完了** |
| M1b | TLS (`%gs`)・`mmap` 完了。シグナルが残り | 一部 |
| M2 | SSE 完了。x87 が残り | 一部 |
| M3 | dyld 相当: 依存解決、chained fixups、stub の thunk 化 | |
| M4 | `objc_msgSend` thunk の自動生成 → 最初の Cocoa アプリ起動 | |
| M5 | basic-block JIT (MAP_JIT + `pthread_jit_write_protect_np`) | |
| M6 | AOT キャッシュ、trace JIT | |

**目標到達は M4**。そこで最初の Cocoa アプリが起動する。M1〜M3 は土台、
M5/M6 は速度改善であって、起動可否には効かない。

## 既知の設計課題

**非 PIE イメージは動かない**
rashid 自身が `0x100000000` に置かれ、x86_64 イメージの希望ベースと衝突する。
リンカでの回避は不可能 (arm64 では `-no_pie` も `-image_base` も無視され、
`-pagezero_size` は 4GB で頭打ち)。PIE なら slide できるので実害はほぼ無い —
2011 年頃以降の x86_64 macOS アプリは全て PIE。非 PIE は明確に拒否する。

**ページサイズ**
x86_64 Mach-O は 4KB 境界、Apple Silicon は 16KB ページ。セグメントを
個別に file-map できないため匿名マップへ copy-in し、権限は領域ごとに
記録してソフトウェアで強制する (`mprotect` は使わない)。1つのホストページが
権限の異なるセグメントにまたがるのが常態なので、丸めると壊れる。
JIT では全アクセス検査ができないため別の答えが要る。

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
