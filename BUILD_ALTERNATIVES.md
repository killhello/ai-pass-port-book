# 不安装 ESP-IDF 的编译方案汇总

共有 4 种方案，按推荐程度排序：

---

## 🥇 方案一：GitHub Actions 云端编译（最推荐）

**优点**：不用装任何东西，浏览器操作即可，免费

**步骤**：

1. 在 GitHub 创建一个新仓库（New Repository）
2. 把 `ai-passport/` 目录下的所有文件推上去：
   ```bash
   cd ai-passport
   git init
   git add .
   git commit -m "ebook reader"
   git branch -M main
   git remote add origin https://github.com/你的用户名/仓库名.git
   git push -u origin main
   ```
3. 打开 GitHub 仓库页面 → 点击 **Actions** 标签
4. 左边选择 **Build Firmware** → 右边点 **Run workflow** → 再点 **Run workflow**
5. 等 3~5 分钟编译完成
6. 进入该次运行详情 → 底部 **Artifacts** 处下载：
   - `ai-passport-ebook-release.zip` —— 打包好的所有 bin + 刷写说明
   - `ai-passport-ebook-firmware.zip` —— 原始构建产物

**已配置文件**：`.github/workflows/build.yml`

---

## 🥈 方案二：PlatformIO（VS Code 插件，一键安装）

**优点**：VS Code 里点几下就装好，自动管理工具链，比 ESP-IDF 省心

**步骤**：

1. 安装 [VS Code](https://code.visualstudio.com/)
2. VS Code 里搜索安装 **PlatformIO IDE** 插件
3. 用 VS Code 打开 `ai-passport` 文件夹
4. 底部状态栏点击 **✓ Build** 按钮（或按 `Ctrl+Alt+B`）
5. 编译完成后固件在 `.pio/build/esp32-c3-devkitm-1/` 目录

**刷写**：连接设备后点底部 **→ Upload** 按钮

**已配置文件**：`platformio.ini`

> 注意：SPIFFS 文件系统上传可能需要额外配置，首次建议用方案一。

---

## 🥉 方案三：Wokwi 浏览器在线模拟

**优点**：不用硬件、不用装工具，直接在浏览器里看效果

**步骤**：

1. 打开 [wokwi.com](https://wokwi.com)
2. 注册登录后，创建一个 **ESP32-C3** 新项目
3. 把 `diagram.json` 和 `wokwi.toml` 的内容复制进去
4. 上传源代码文件
5. 点击 **Play** 按钮运行模拟

**已配置文件**：`diagram.json`、`wokwi.toml`

> 注意：Wokwi 更适合调试 UI 和逻辑，不能完全替代真实硬件测试。
> SPIFFS 在 Wokwi 中可能需要特殊处理。

---

## 🏅 方案四：ESP-IDF 离线安装包（Windows 一键安装）

**优点**：官方工具，功能最全，一次安装永久使用

**下载地址**：
https://dl.espressif.com/dl/esp-idf/

选择 **v5.5.3** 的离线安装包（offline installer），约 1.5GB，下载后双击安装即可。

安装完成后从开始菜单打开 **ESP-IDF 5.5 PowerShell**，然后：
```bash
cd ai-passport
idf.py set-target esp32c3
idf.py build spiffsgen flash monitor
```

---

## 各方案对比

| 方案 | 需要安装 | 需要硬件 | 编译速度 | 难度 | 推荐度 |
|------|---------|---------|---------|------|--------|
| GitHub Actions | 无 | 需要 | 3~5 分钟 | ⭐ | ⭐⭐⭐⭐⭐ |
| PlatformIO | VS Code + 插件 | 需要 | 2~4 分钟 | ⭐⭐ | ⭐⭐⭐⭐ |
| Wokwi 模拟 | 无 | 不需要 | 即时 | ⭐ | ⭐⭐⭐ |
| ESP-IDF 完整 | ~2GB | 需要 | 1~3 分钟 | ⭐⭐⭐ | ⭐⭐⭐ |

---

## 最快上手路径

1. 把代码推到 GitHub
2. Actions 里点一下 Run workflow
3. 下载 zip
4. 用 [ESP Web Flasher](https://esphome.github.io/esp-web-tools/) 在线刷写（连 USB，浏览器直接刷，不用装 esptool）
