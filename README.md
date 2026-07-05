# AviUtl ExEdit2 relink

AviUtl ExEdit2 のプロジェクトファイル (.aup2) が参照しているファイルのパスを確認・修正するツールです。

動画ファイルや素材ファイルを別のドライブやフォルダに移動したとき、プロジェクト内の参照パスをまとめて書き換えられます。

エイリアスファイル(.object)でも使えます。

## 注意事項

- 無保証です。自己責任で使用してください。  
このツールを利用したことによる、いかなる損害・トラブルについても責任を負いません。

## 機能

- プロジェクトが参照しているファイルの一覧表示
- ファイルの存在チェック（OK / NG 表示）
- ファイルの存在チェック結果のCSV出力
- 表示の絞り込み（キー/パス検索、NGのみ表示）
- パスの個別変更（同じファイルを参照している行をまとめて変更可能）
- 書き換え前の自動バックアップ（`.aup2.bak`）
- 読み込み後に `.aup2` が外部更新された場合の保存前警告
- プロジェクトファイル自体の記録パスと実際のパスの不一致検出
- 参照素材のコピー（外部設定ファイルでコピー/除外/確認ルールを編集可能）
- プロジェクト直下へコピー済みの素材フォルダからの再リンク

## 動作要件

- Visual C++ 再頒布可能パッケージ（v14 の x64 対応版が必要）
  - <https://learn.microsoft.com/ja-jp/cpp/windows/latest-supported-vc-redist>

## 使い方

1. `aviutl2_relink.exe` を起動します  
  AviUtl2内の編集->プラグイン->aviutl2_relinkからも開けます
2. 「開く」ボタンまたはドラッグ＆ドロップで `.aup2` ファイルを開きます
3. 参照ファイルの一覧が表示されます

### ファイルの存在チェック

「チェック」ボタンを押すと、一覧の各ファイルが実際に存在するか確認します

- `OK` — ファイルが見つかった
- `NG` — ファイルが見つからない

「チェック結果出力」ボタンを押すと、存在チェックを実行したうえで結果をCSVに保存できます。  
CSVには、各参照ファイルの `status`、`key`、`path`、`is_project_file`、`project` が出力されます。

絞り込み欄に文字を入力すると、キーまたはパスに一致する行だけを表示できます。  
「NGのみ」を有効にすると、存在チェック後に見つからなかった参照だけを表示できます。

### パスの変更

1. 一覧から変更したい行を選択します
2. 「選択行を変更」ボタンを押してファイルダイアログで新しいパスを指定します
3. 同じファイルを参照している行が他にある場合、まとめて変更するか確認ダイアログが出ます

### 保存

「保存」ボタンを押すと `.aup2` ファイルに書き戻します  
書き戻しの直前に `.aup2.bak` が自動作成されます

読み込み後に別のアプリケーションで `.aup2` が更新されていた場合は、保存前に確認が表示されます。

### 参照素材のコピー

「素材をコピー」ボタンを押すと、プロジェクトが参照しているファイルを指定フォルダへコピーできます。  
コピー後に、プロジェクト内の参照パスをコピー先へ変更することもできます。

コピー対象の判定は、実行ファイルと同じフォルダにある `aviutl2_relink.copy.ini` で設定します。  
設定ファイルがない場合は初回実行時に自動作成されます。

### プロジェクト直下素材への再リンク

「直下素材へ再リンク」ボタンを押すと、移動後の `.aup2` と同じ場所にある素材フォルダを基準に、参照パスを現在の絶対パスへ書き換えます。

`update_paths_after_copy` で参照パスをコピー先へ変更したあと、`.aup2` と `{project_name}_files` フォルダを別PCや別フォルダへまとめて移動した場合に使います。  
`.aup2` 内に残っている旧素材フォルダ以降のパスを使うため、コピー時の `preserve_tree` が `flat` / `drive` / `common_root` のどれだったかを合わせ直す必要はありません。

例:

```text
移動前:
C:/work/demo.aup2
C:/work/demo_files/bg.png

移動後:
E:/backup/demo.aup2
E:/backup/demo_files/bg.png

再リンク後の参照パス:
E:/backup/demo_files/bg.png
```

実行前に、置換件数、使用する素材フォルダ、置換例が確認表示されます。  
実行後は未保存状態になります。保存時には通常通り `.aup2.bak` が作成されます。

設定例:

```ini
default_action=ask
preserve_tree=drive
project_side_folder={project_name}_files
update_paths_after_copy=ask
overwrite=ask
export_log=ask
export_result=ask
save_after_update=ask

skip=*.dll|binary/plugin
ask=*.lua|script
copy=*.wav
copy=*.png
copy=C:/素材/**
```

- `copy=pattern` — 一致したファイルをコピーします
- `skip=pattern` — 一致したファイルを除外します
- `ask=pattern` — 実行時にコピーするか確認します
- `default_action` — どのルールにも一致しなかった場合の動作です（`copy` / `skip` / `ask`）
- `preserve_tree` — コピー先の配置方法です（`flat` / `drive` / `common_root`）
- `project_side_folder` — プロジェクトファイル直下へコピーする場合のフォルダ名です
- `update_paths_after_copy` — コピー後に参照パスを更新するかの動作です（`yes` / `no` / `ask`）
- `overwrite` — コピー先に同名ファイルがある場合の動作です（`yes` / `no` / `ask`）
- `export_log` — コピー後にログファイルを出力するかの動作です（`yes` / `no` / `ask`）
- `export_result` — コピー後に結果CSVを出力するかの動作です（`yes` / `no` / `ask`）
- `save_after_update` — 参照パス更新後に `.aup2` を保存するかの動作です（`yes` / `no` / `ask`）

ルールは上から順に評価されます。  
`*.wav` のようにスラッシュを含まないパターンはファイル名に対して、`C:/素材/**` のようにスラッシュを含むパターンはフルパスに対して判定されます。

#### ルールの動作

`copy` / `skip` / `ask` は、どれも同じ形式で書きます。

```ini
copy=*.wav
skip=*.dll|binary/plugin
ask=*.lua|script
```

`|` の右側は理由メモです。省略できます。  
`skip` や `ask` の理由は、確認ダイアログの表示に使われます。

ルールは上から順に評価され、最初に一致したルールだけが使われます。

```ini
skip=C:/Program Files/**
copy=*.wav
```

この場合、`C:/Program Files/Vendor/sample.wav` は `*.wav` にも一致しますが、先に `skip=C:/Program Files/**` に一致するため除外されます。

#### パターンの書き方

パターンには `*`、`**`、`?` が使えます。大文字小文字は区別しません。

- `*.wav` — 拡張子が `.wav` のファイルに一致します
- `voice_??.png` — `voice_01.png` や `voice_ab.png` に一致します
- `C:/素材/**` — `C:/素材/` 以下のファイルに一致します
- `D:/work/*.mp4` — `D:/work/` 直下の `.mp4` に一致します

`*` と `?` はフォルダ区切り `/` をまたぎません。  
サブフォルダ以下も含めたい場合は `**` を使います。

スラッシュ `/` またはドライブ指定 `C:` を含むパターンはフルパスに対して判定されます。  
それ以外のパターンはファイル名だけに対して判定されます。

#### `default_action`

どのルールにも一致しなかったファイルの扱いです。

- `copy` — 既知/未知を問わずコピーします
- `skip` — ルールに書いたものだけをコピー対象にします
- `ask` — ルールにないものは実行時に確認します

安全寄りに使うなら `ask`、素材拡張子を明示管理したいなら `skip` が向いています。

#### `preserve_tree`

コピー先でフォルダ構造をどう作るかを指定します。  
例として、コピー先が `C:/archive/sample_files/` で、参照ファイルが以下の場合:

```text
D:/素材/voice/a.wav
D:/素材/image/bg.png
E:/download/bg.png
```

`flat` は、すべてコピー先直下に置きます。

```text
C:/archive/sample_files/a.wav
C:/archive/sample_files/bg.png
C:/archive/sample_files/bg (2).png
```

同名ファイルがある場合は、コピー計画上で `bg (2).png` のように名前を分けます。  
ファイル数が少ない時は見やすいですが、元の場所の情報は失われます。

`drive` は、ドライブ名から下の構造を残します。

```text
C:/archive/sample_files/D_/素材/voice/a.wav
C:/archive/sample_files/D_/素材/image/bg.png
C:/archive/sample_files/E_/download/bg.png
```

同名ファイルの衝突を避けやすく、別ドライブの素材も混ざりにくいです。初期値はこれです。

`common_root` は、コピー対象の共通フォルダからの相対パスを残します。

```text
C:/archive/sample_files/voice/a.wav
C:/archive/sample_files/image/bg.png
```

上の2つだけが対象なら共通フォルダは `D:/素材/` なので、その下の構造だけを残します。  
ただし、複数ドライブにまたがるなど共通フォルダがうまく取れない場合は `drive` 相当の配置になります。

#### `project_side_folder`

「プロジェクトファイル直下にコピー」を選んだ時に作るフォルダ名です。  
`{project_name}` は `.aup2` のファイル名に置き換わります。

```ini
project_side_folder={project_name}_files
```

`C:/work/demo.aup2` なら、コピー先は `C:/work/demo_files/` になります。

#### `update_paths_after_copy`

コピーに成功したファイルについて、`.aup2` 内の参照パスをコピー先に書き換えるかを指定します。

- `yes` — 常に書き換えます
- `no` — コピーだけ行い、参照パスは変えません
- `ask` — 実行時に確認します

書き換えた場合は未保存状態になります。保存時には通常通り `.aup2.bak` が作成されます。

#### `save_after_update`

コピー後に参照パスを書き換えた場合だけ、`.aup2` を保存するかを指定します。  
参照パスを変更しなかった場合や、変更件数が0件だった場合は保存しません。

- `yes` — 参照パス更新後に自動保存します
- `no` — 自動保存しません
- `ask` — 参照パス更新後に確認します

保存時には通常通り `.aup2.bak` が作成されます。

#### `overwrite`

コピー先に同名ファイルがすでにある場合の動作です。

- `yes` — 上書きします
- `no` — 既存ファイルはスキップします
- `ask` — 実行時に確認します

#### `export_log`

コピー後に、人間が読むためのログを `.log` で出力するかを指定します。

- `yes` — 常に出力します
- `no` — 出力しません
- `ask` — コピー後に確認します

ログには、コピー元、コピー先、コピー件数、失敗件数、除外理由、参照パス更新件数、自動保存結果が含まれます。  
出力先はコピー先フォルダで、ファイル名は `{project_name}_copy_YYYYMMDD_HHMMSS.log` です。

#### `export_result`

コピー後に、表計算ソフトで確認しやすい結果CSVを出力するかを指定します。

- `yes` — 常に出力します
- `no` — 出力しません
- `ask` — コピー後に確認します

CSVには、各参照ファイルの `result`、`action`、`source`、`destination`、`reason`、`detail` が出力されます。  
出力先はコピー先フォルダで、ファイル名は `{project_name}_copy_YYYYMMDD_HHMMSS.csv` です。

## 改版履歴

- **v0.0.8**
  - 見れる情報を追加
  - .objectファイルに対応

- **v0.0.7**
  - 設定ファイルの作成エラー時のメッセージを追加
  - ウィンドウサイズに最小値を追加
  - 一部処理を非同期にしてUIが固まらないように変更
  - コピー処理時のクラッシュを修正
  - CSVを出力した際のインジェクション対策を追加
  - UIを変更し高DPIに対応

- **v0.0.6**
  - 元ディレクトリが存在しなくても移動できるように変更

- **v0.0.5**
  - aux2のランチャーを同梱

- **v0.0.4**
  - 直下にコピーした素材の再リンク機能を追加

- **v0.0.3**
  - 結果の出力機能を追加
  - 素材のコピー機能を追加

- **v0.0.2**
  - バッファオーバーランによってクラッシュする問題を修正
  - 未保存の変更があるさいに確認を追加
  - ディレクトリの一括置換を追加

- **v0.0.1**
  - 初版

## License

このプロジェクトはMIT Licenseの下で公開されています

### 前提条件

- Visual Studio 2026
- Git

### 実際の手順

1. `git clone https://github.com/Book-0225/aviutl2_relink`
2. `cd aviutl2_relink`
3. `msbuild aviutl2_relink.vcxproj /p:Configuration=Release /p:Platform="x64"`

### AviUtl2 汎用プラグインランチャー

`aviutl2_relink_launcher.vcxproj` をビルドすると、AviUtl2 の編集メニューから
`aviutl2_relink.exe` を起動するための汎用プラグイン
`aviutl2_relink_launcher.aux2` が作成されます。

```text
msbuild aviutl2_relink_launcher.vcxproj /p:Configuration=Release /p:Platform="x64"
```

`aviutl2_relink_launcher.aux2` と `aviutl2_relink.exe` を同じフォルダに配置してください。
AviUtl2 側で未保存の変更がある場合、外部ツールには最後に保存した `.aup2` の内容だけが表示されます。

編集メニューには以下が追加されます。

- `aviutl2_relink > 現在のプロジェクトを開く`
- `aviutl2_relink > aup2 を選んで開く`
- `aviutl2_relink > ツールだけ起動`

## Credits

### AviUtl ExEdit2 Plugin SDK

```
---------------------------------
AviUtl ExEdit2 Plugin SDK License
---------------------------------

The MIT License

Copyright (c) 2025 Kenkun

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```
