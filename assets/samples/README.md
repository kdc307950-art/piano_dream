# 钢琴采样目录

把钢琴采样文件放进这个目录，程序启动时会自动扫描加载；
目录内已内置 FreePats Kawai Upright Piano 的 CC0 FLAC 真实录音；如果采样缺失或解码失败，程序自动使用内置的离线合成音色。

## 命名规则

**文件名（去掉扩展名）就是锚点音高**，支持这些写法：

```
C4.wav        音名
Ds4.wav       升号用 s（避免 # 在文件名里的兼容问题）
Eb4.wav       降号用 b
060.wav       直接写 MIDI 编号（21 = A0，108 = C8）
Piano_C4_1.wav   前后带前缀后缀也能识别
```

支持格式：WAV / FLAC / MP3 / OGG / M4A / AIFF（由 miniaudio 解码）。

## 锚点密度

程序对每个音高选取**最近的锚点**做变调（`playbackRate = 2^((note-anchor)/12)`）。
距离超过 12 个半音会自动回退到合成音色。

- 锚点越密，音质越自然；建议**每 3~4 个半音一个锚点**
- 最低限度：每个八度一个 C（`C1.wav` ~ `C8.wav`）

## 关于许可

当前随包采样来自 [FreePats Upright Piano KW](https://github.com/freepats/upright-piano-KW)，
录制对象为 Kawai 立式钢琴，发布许可为 CC0 1.0。来源元数据与许可证副本见：

- `FREEPATS_SOURCE.json`
- `FREEPATS_CC0_LICENSE.txt`
- `FREEPATS_README.md`

公开发布时请保留上述来源文件，便于追溯资源版本。
文件名中的 `vL` / `vH` 分别对应低、高力度层，程序会根据实际演奏力度选择更接近的录音层。

## 建议的采样库

| 名称 | 特点 | 许可 |
|---|---|---|
| FreePats Upright Piano KW | 当前内置，真实立式钢琴录音 | CC0 1.0 |
| Salamander Grand Piano | 音质好，锚点密集（每 3 个半音） | CC-BY 3.0（需署名） |

替换或增加其他采样时，重命名成上面的规则即可，例如：

```
C1.wav  Ds1.wav  Fs1.wav  A1.wav  C2.wav  ...
```
