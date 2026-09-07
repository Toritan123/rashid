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
- **動的ロード**。`LC_DYLD_CHAINED_FIXUPS` を辿り、イメージを rebase し、
  同一プロセスに既にロードされているネイティブ arm64 フレームワークに対して
  全 import を解決する
- **C 関数の thunk**。翻訳されたコードがネイティブ arm64 の libSystem を
  呼び、正しい結果を受け取る。可変長引数を含む
- **Objective-C**。セレクタをネイティブランタイムに登録し、クラスと定数文字列を
  実オブジェクトに bind し、メッセージ送信をマシン上の arm64 Foundation で走らせる
- **ゲストコードへのコールバック**。ネイティブフレームワークが翻訳された
  コードを呼び戻せる — `qsort` のコンパレータ、メソッド実装
- **アプリ自身が定義するクラス**。インスタンス変数、クラスメソッド、
  `super` 呼び出しを含む
- クラス判定付きの syscall: `read` / `write` / `close` / `exit` /
  `thread_fast_set_cthread_self`

動かないもの: AppKit と GUI 全般、カテゴリ、プロトコル、ブロック、
AVX、x87、シグナル、スレッド、ファイル実体を伴う `mmap`。

Objective-C バイナリが最後まで動き、実機と同一の出力を出す。

```
len=23              range=6,9           double=2.50
prefix=1            count=3 first=x     dict=v
format=built/7      NSLog reached with hello objective-c world (23)
```

### Objective-C は予想より簡単で、予想外に難しかった

`objc_msgSend` には特別扱いが一切要らなかった。可変長引数に見えるが違う。
コンパイラは各呼び出し地点の本当のシグネチャを知っていて通常の呼び出しを
出すので、System V と AAPCS64 のレジスタ割り当てが一致し、汎用 thunk が
そのまま通る。

必要だったのは、本来 dyld がランタイムに伝える内容のほう。イメージ内の
セレクタ参照はそのイメージ自身の文字列を指しており、`objc_msgSend` は
セレクタを文字列ではなくポインタで比較するため、ロード時に全て
ネイティブランタイムへ登録し直す。クラス参照と定数文字列は、データ
シンボルの直接 bind で既に解決している。

本当に作業が要ったのは3点。いずれも動かして誤りを目撃して見つけた:

- 16バイト構造体は片方が `rax:rdx`、もう片方が `x0:x1` で返る。`x0` しか
  拾っていなかったので `[s rangeOfString:]` が「もっともらしい位置と
  ゴミの長さ」を返した
- `NSLog` の書式は C 文字列ではなく **NSString**。読むにはオブジェクトに
  バイト列を尋ねる必要がある — 相手はネイティブなので普通のメッセージ送信
- `stringWithFormat:` など一部の ObjC メソッドは可変長引数を取る。メソッドの
  型エンコーディングは宣言された引数しか記述しないので可変長性は metadata
  から判別できず、セレクタを一覧で引くしかない

### アプリが定義するクラス

クラスは公開ランタイム API — allocate / add methods / register — で構築し、
実装は全て trampoline に差し替える。実装は x86_64 コードなので、ランタイムを
直接そこへ入らせるわけにいかない。インスタンス変数も同様に宣言し、ランタイムが
決めたオフセットをイメージ側へ書き戻す。コンパイル済みの ivar アクセスは
そこを読むため。

残るのは同一性の問題。イメージは自分のクラスをアドレスで参照する。clang は
`_OBJC_CLASS_$_Foo` への直接の `leaq` を出すので、差し替えるべき間接参照が
ない。素直な解 — 完成したクラスオブジェクトをイメージ側へ複写する — は
**通用しない。libobjc は arm64e であり、ポインタをアドレス依存で署名している**。
別アドレスへ移したクラスオブジェクトはもう有効ではなく、ランタイムがそう言う。

そこでイメージ側の構造体はハンドルとして扱う。それがどの実クラスを表すかを
rashid が覚えておき、ゲストが境界を越えて渡すときに本物へ差し替える。
同一性が観測されうるのはそこだけである。

```
hits=5          describe=clicks=5     version=1.0
isa=Counter     responds=1            byname=Counter
```

`RASHID_OBJC_DEBUG=1` で何を見つけ何を登録したかが出る。

### 逆方向の呼び出し

thunk は翻訳されたコードがフレームワークを呼ぶための仕組み。アプリには逆も
要る。`qsort` はコンパレータを呼び、フレームワークはデリゲートを呼び、
Objective-C ランタイムはメソッド実装を呼ぶ。いずれも arm64 の呼び出しとして
到着し、x86_64 のインタプリタに行き着かねばならない。

rashid はアセンブルされた trampoline の並びからアドレスを配る。各 trampoline
は自分の番号を読んで共通のディスパッチャへ飛び、ディスパッチャが引数
レジスタと呼び出し元のスタックを捕らえ、インタプリタの状態を退避し、ゲスト
関数を走らせ、結果を AAPCS64 の期待する場所に戻す。実行時生成ではなく
アセンブル済みなので、`MAP_JIT` も W^X の手当ても要らない。

ネイティブ関数のどの引数がコールバックかはシグネチャなしには分からないので、
`qsort`・`bsearch`・`atexit` など一般的なものを列挙している。可変長引数と
同じ欠落であり、同じ解決策を待っている。

### ABI 境界を越える

整数の規約はほぼ一致している。System V の `rdi, rsi, rdx, rcx, r8, r9` が
AAPCS64 の `x0..x5` になり、浮動小数点は片方が `xmm0-7`、もう片方が
`v0-v7`。小さなアセンブリの呼び出しゲートが、選んだレジスタとスタック状態で
ネイティブ関数に入り、`x0` と `d0` の両方を回収する。どちらの返り値レジスタが
意味を持つかは、rashid が知らないかもしれないシグネチャ次第だからである。

import されたシンボルがコードかデータかで bind 方法が変わる。関数は stub を
経由させねばならない。直接 bind するとゲストが arm64 命令を x86 として
実行しようとするため。データ — Objective-C のクラスオブジェクト、
CoreFoundation の定数 — は呼ばれず参照されるだけなので、ネイティブアドレスに
直接 bind する。名前から推測するのではなく、シンボルを自身のイメージ内で
引いて、含まれる**セクション**が命令を持つかで判定する。セグメントでは
足りない。`__TEXT` は実行可能だが読み取り専用データも抱えており、
全クラスが指す `__objc_empty_cache` は `(__TEXT,__const)` にある。

可変長引数は本当の作業を要する。**macOS arm64 は可変長引数を全てスタックで
渡す**が、System V は最初の数個をレジスタで渡す。転送するには引数の個数と
型を知る必要があり、書式付き入出力の一群についてはそれが書式文字列から
分かるので、rashid は書式を解析して呼び出しを組み直す。それ以外の可変長
関数は既知の欠落である。シグネチャなしではレジスタで渡してしまい誤動作する。

### 最初の1本まであとどれくらいか

Cocoa の hello world (`NSString`、`NSLog`、autorelease pool) が import する
シンボルは **8個**:

```
_NSLog                                Foundation
_OBJC_CLASS_$_NSString                Foundation
_objc_autoreleasePoolPop              libobjc
_objc_autoreleasePoolPush             libobjc
_objc_msgSend                         libobjc
_objc_release                         libobjc
_objc_retainAutoreleasedReturnValue   libobjc
___CFConstantStringClassReference     CoreFoundation
```

これが最初のマイルストーンの実際の大きさ。先に数えた C 関数 32,444 個は
最終的な表面積であって入場料ではない。アプリが触るのはその小さな、そして
大部分が共通の部分集合にすぎない。8個のうち6個は Objective-C ランタイムの
入口であり、`objc_msgSend` を**通った先**は手書きではなく生成された thunk
が担う。

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
| M3 | chained fixups・bind・C thunk | **完了** |
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
