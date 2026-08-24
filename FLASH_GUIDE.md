# FoloToy AI Passport 电子书阅读器 - 构建与刷写指南

## 前置要求

- ESP-IDF v5.5.x（推荐 v5.5.3）
- Python 3.8+
- 8MB Flash 的 FoloToy AI Passport 硬件

## 一、安装 ESP-IDF

### Windows

1. 下载 [ESP-IDF Tools Installer](https://dl.espressif.com/dl/esp-idf/)
2. 安装时选择 v5.5 版本
3. 安装完成后，从开始菜单打开 "ESP-IDF 5.5 PowerShell" 或 "ESP-IDF 5.5 CMD"

### Linux / macOS

```bash
sudo apt-get install git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
mkdir -p ~/esp
cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git -b v5.5.3
cd esp-idf
./install.sh esp32c3
. ./export.sh
```

## 二、构建固件

### 1. 设置目标芯片（首次构建）

```bash
cd ai-passport
idf.py set-target esp32c3
```

### 2. 编译固件

```bash
idf.py build
```

编译成功后，固件文件位于 `build/` 目录：
- `build/FoloToy-AI-Passport.bin` —— 主固件（应用程序）
- `build/bootloader/bootloader.bin` —— Bootloader
- `build/partition_table/partition-table.bin` —— 分区表

### 3. 构建 SPIFFS 镜像（电子书数据分区）

```bash
python $IDF_PATH/components/spiffs/spiffsgen.py 0x4F0000 spiffs_data build/spiffs.bin \
  --page-size 256 --block-size 4096 \
  --obj-name-len 32 --meta-len 4 \
  --use-magic --use-magic-length --use-mtime
```

> 参数必须与 sdkconfig.defaults 中 SPIFFS 配置一致（OBJ_NAME_LEN=32、META_LEN=4、
> USE_MAGIC、USE_MAGIC_LENGTH、USE_MTIME），否则设备上会挂载失败。

SPIFFS 镜像输出：
- `build/spiffs.bin` —— SPIFFS 文件系统镜像

### 4. 合并为单文件镜像（推荐，避免漏刷）

```bash
python -m esptool --chip esp32c3 merge-bin -o build/flash_all.bin \
  --flash_mode dio --flash_freq 40m --flash_size 8MB \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/FoloToy-AI-Passport.bin \
  0x310000 build/spiffs.bin
```

GitHub Actions 构建产物已自动包含 `flash_all.bin`。

> 注意：电子书文件需要放在 `spiffs_data/` 目录下，文件名为 `book.txt`。
> 当前已提供示例文件 `spiffs_data/book.txt`（约 30KB）。

## 三、刷写固件

### 方式一：一条命令全部刷写（推荐）

```bash
idf.py flash monitor
```

这会自动刷写 bootloader、分区表、应用固件和 SPIFFS，并打开串口监视器。

### 方式二：单文件刷写（GitHub 产物，推荐）

```bash
# 先擦除一次,清除旧固件/旧分区表残留(强烈建议)
esptool.py --chip esp32c3 -p COM3 erase_flash

# 一次性刷入全部内容
esptool.py --chip esp32c3 -p COM3 -b 460800 write_flash 0x0 flash_all.bin
```

### 方式三：分别刷写各部分

```bash
# 刷写 bootloader (偏移 0x0)
esptool.py -p COM3 -b 460800 write_flash 0x0 build/bootloader/bootloader.bin

# 刷写分区表 (偏移 0x8000) —— 必须刷!漏刷会导致无限重启
esptool.py -p COM3 -b 460800 write_flash 0x8000 build/partition_table/partition-table.bin

# 刷写主固件 (偏移 0x10000)
esptool.py -p COM3 -b 460800 write_flash 0x10000 build/FoloToy-AI-Passport.bin

# 刷写 SPIFFS 数据分区 (偏移 0x310000)
esptool.py -p COM3 -b 460800 write_flash 0x310000 build/spiffs.bin
```

> 将 `COM3` 替换为实际的串口号。

### 方式四：仅更新电子书（不刷固件）

如果只是想替换电子书内容，只需重新生成并刷写 SPIFFS 分区：

```bash
# 1. 把新的 book.txt 放到 spiffs_data/ 目录
# 2. 重新生成 SPIFFS 镜像(命令见上文,含完整参数)
# 3. 只刷写 SPIFFS 分区
esptool.py --chip esp32c3 -p COM3 write_flash 0x310000 build/spiffs.bin
```

## 四、分区表布局

| 名称 | 类型 | 偏移地址 | 大小 | 说明 |
|------|------|----------|------|------|
| nvs | data | 0x9000 | 24 KB | 配置/阅读进度保存 |
| phy_init | data | 0xf000 | 4 KB | PHY 初始化数据 |
| factory | app | 0x10000 | 3072 KB | 主应用固件 |
| spiffs | data | 0x310000 | ~4980 KB | 电子书存储 |

总 Flash: 8MB (0x800000)

## 五、串口监视器

```bash
idf.py monitor
```

退出监视器：按 `Ctrl+]`

## 六、常见问题

### 1. 编译报错找不到组件

```bash
idf.py fullclean
idf.py build
```

### 2. 刷写失败

- 检查 USB 线是否连接正常
- 确认串口号正确
- 按住 OK 键再插入 USB（进入下载模式）
- 降低波特率：`idf.py -b 115200 flash`

### 3. 电子书找不到

- 确认 `spiffs_data/book.txt` 存在
- 确认已刷写 SPIFFS 分区
- 串口日志中查看 "SPIFFS 已挂载" 相关输出

### 4. 如何添加自己的电子书

1. 将 `.txt` 文件（UTF-8 编码）重命名为 `book.txt`
2. 放入 `spiffs_data/` 目录
3. 执行 spiffsgen 命令生成镜像(见上文,含完整参数)
4. 执行 `esptool.py write_flash 0x310000 build/spiffs.bin` 刷写

## 七、按键操作

| 按键 | 功能 |
|------|------|
| UP | 上一页 |
| DOWN | 下一页 |
| OK（短按） | 息屏 / 亮屏 |
| OK（长按） | 返回主菜单 |

阅读进度会在每次翻页时自动保存到 NVS，下次打开自动恢复。
