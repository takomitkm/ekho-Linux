# ekho —— 录音拼接式 TTS（**现役交付路线**）

> **代码不在这里。** 正本在 `repos/ekho-linux`（本仓 fork）。本目录只记录
> **它在这块板上的定位、读数，以及它与神经 TTS 的关系**。

---

## 1. 为什么它在这条链上有一席之地

板子只有 **993 MB RAM、4×Cortex-A53 @1 GHz**。神经 TTS 的现实是：

| 方案 | RTF | 内存 | 结果 |
|---|---:|---:|---|
| Piper / Kokoro / VITS 族 | 0.65–0.81 常驻（快于实时）；**kokoro 例外：RTF≈8.2，真慢** | 391–755 MB 峰值 | **OOM 被杀 12 次** |
| **ekho**（拼接） | **0.49** | **~20 MB RSS** | **可用** |
| matcha+vocos（两段式，vocos 上 NPU） | **0.716–0.777**（快于实时） | — | **可用，但需 NPU 参与** |

**ekho 的内存优势是数量级的**（20 MB vs 数百 MB），单核就快于实时。
在这块板上，**"中文能实时"和"神经网络"这两件事目前是互斥的**，ekho 是绕开算力预算的那条路。

---

## 2. 板上实测

| 项 | 值 |
|---|---|
| 二进制 | `/root/ekho-bench/bin/ekho`，**2.6 MB** |
| 版本 | **11.0-linux.1** |
| **RTF** | **0.49** |
| **RSS** | **~20 MB** |
| 包采样率 | 48 kHz（升级后已设默认） |

> 注意 48 kHz 这件事：**"直接生成 48 kHz"在开源神经模型里几乎不存在**（Piper、CosyVoice2、
> F5-TTS 都是 24 kHz 级）。ekho 的 48 kHz 来自**录音包**，不是模型幻觉补出来的，
> 所以那是真信息，不是插值出来的细节。

---

## 3. 它是什么技术路线

**录音拼接（unit selection / concatenative）**：
预先录好音节/词条音频，合成时按文本拼接 + 做韵律调整。

- **优点**：零 GPU/NPU、内存极小、延迟稳定
- **缺点**：音色固定、覆盖有限（未登录词只能靠规则变调）、听感受录音质量约束
- 学术谱系：与 MBROLA（diphone）同类；espeak-ng 的 `--pho` → MBROLA 是另一条

---

## 4. 与神经 TTS 的关系（一句话）

```text
ekho       ：文本 ──[ 录音拼接 ]──────────────> 波形      ← 不需要 NPU
matcha+vocos：文本 ──[ 前端 ][ 声学模型 ][ 声码器 ]──> 波形  ← 只有声码器那段值得搬 NPU
piper      ：文本 ──[ 单体 VITS ]─────────────> 波形      ← 切不开，板子放不下
```

**ekho 不是"更差的神经 TTS"，是"另一类技术"**——所以它不参与"哪层该放 NPU"的讨论。

---

## 5. 上游活跃度（A 级，2026-10-21 全量 clone 实测）

- 上游 `hgneng/ekho`，master 分支就叫 `master`
- 历史自 **2016-04-14** 起，共 **359 commits**，最新 `c7b8ef6` **2026-09-02**
- 近 12 个月 **13 commits**
- **它已经在接神经**：`2025-06-25` 支持 zhtts、`2026-03-25` **支持 piper**、
  `2026-04-08` 支持英文经 piper、`2026-04-22` 修 `enablePiper` 崩溃

⇒ **上游不是冻土**。README 冻在 2023-09-04 只是门面没更新，活体在 commits 流。

---

## 6. 本仓 fork 相对上游做了什么

基底 `61392db`（2026-10-14，"Linux-only fork, streaming API, ALSA backend"），上续 27 笔到 `d043e07`。
净增面：

- Linux 干净可编（`ENABLE_ESPEAK` 重定义等）
- 合成回调路径修复 + ALSA/streaming/归一化
- **`ekho-server` HTTP/1.1 流式 TTS**（FunASR 接口面的地基）
- 英文速率/音色旋钮按听感定档（en-us+f3、100 wpm）
- 语音块**内存解码**（废临时文件）
- aarch64 交叉与 kit 全管线（校机 MSYS2 + L1 三闸 + tar 自校）
- 包升级 48 kHz 并设默认
- 字母采样随包率
- vendored 许可证清单与二进制许可口径

详见 `repos/ekho-linux` 的 README 与提交历史。

---

## 7. 相关

- 代码正本：`repos/ekho-linux`
- 板上读数与取舍：`wiki/PS/Backend/mods/audio/实时伪tts.md`
- 与神经 TTS 的对照：`wiki/PS/Backend/mods/audio/ARM CPU TTS性能测试.md`
- 决策：`wiki/archive/开发管理/状态源与决策记录.md`（TTS 现役路线相关条目）