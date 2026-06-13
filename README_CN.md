# lanft — 局域网文件传输工具
[![Version](https://img.shields.io/badge/version-v1.0.0-blue.svg)](https://gitee.com/dzh258/lan-file-transfer/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Gitee stars](https://gitee.com/dzh258/lan-file-transfer/badge/star.svg?theme=dark)](https://gitee.com/dzh258/lan-file-transfer)
[![GitHub stars](https://img.shields.io/github/stars/Spinach5/lan-file-transfer?style=social)](https://github.com/Spinach5/lan-file-transfer)

[English](./README.md) | 中文

一款高速、稳定的局域网文件传输工具，同时支持图形界面（基于SDL2）和命令行模式。支持TCP/UDP协议、目录压缩、断点续传、设备扫描以及传输记录功能。

---

## 功能特性
- **双运行模式**：图形界面（SDL2）+ 命令行终端
- **TCP & UDP 协议**：TCP可靠流式传输；UDP基于应答机制并支持重传
- **目录传输**：借助libarchive自动打包为tar.gz格式，接收端自动解压
- **断点续传**：检测未完成文件，从中断位置继续传输
- **局域网设备扫描**：多线程TCP连接探测，适配所有网卡
- **传输记录**：本地持久化保存传输速度、进度、状态等日志
- **类netcat角色逻辑**：接收端为服务端（监听端口），发送端为客户端（主动连接）
- **跨平台**：支持Linux（x86_64、aarch64）、安卓Termux、Windows子系统(WSL)

---

## 项目结构
```
websocket/
├── CMakeLists.txt              # 构建配置文件
├── README.md
├── .gitignore
├── docs/
│   └── superpowers/
│       ├── specs/              # 设计规范文档
│       └── plans/              # 开发实现方案
└── src/
    ├── main.c                  # 程序入口、SDL事件循环、线程创建
    ├── cli.c                   # 命令行参数解析与终端传输逻辑
    ├── network.h / network.c   # 基于libwebsockets实现原生TCP、标准BSD UDP
    ├── scanner.h / scanner.c   # 多线程局域网TCP设备扫描模块
    ├── transfer.h / transfer.c # 文件收发、元数据交互、断点续传、压缩解压
    ├── protocol.h              # 通用常量、数据结构体、事件类型定义
    └── ui.h / ui.c             # SDL2图形界面：标签页、按钮、输入框、进度条
```

### 模块职责
| 模块 | 作用 | 依赖项 |
|--------|---------|-------------|
| `main.c` | SDL2窗口管理、事件循环、标签页状态、线程调度 | ui、network、scanner、transfer |
| `cli.c` | 命令行参数解析（getopt_long）、进度展示、同步传输 | network、transfer |
| `network.c` | 基于libwebsockets实现TCP原生套接字；通过BSD接口sendto/recvfrom实现UDP | libwebsockets、protocol.h |
| `scanner.c` | 筛选所有非回环/24位子网，启用32线程执行TCP连通性扫描 | pthreads、SDL（事件） |
| `transfer.c` | 元数据握手、分块收发、断点偏移校验、libarchive压缩解压 | network.h、protocol.h、libarchive |
| `ui.c` | 多标签页面（扫描/发送/接收/记录）、输入组件、进度条、8×16点阵字体 | SDL2 |
| `protocol.h` | 校验魔数、默认端口、数据包头、事件载荷结构体 | 无 |

---

## 依赖库
### 必需依赖
| 库名称 | 版本要求 | 用途 |
|---------|---------|---------|
| SDL2 | ≥ 2.0 | 图形界面渲染与事件响应 |
| libwebsockets | ≥ 4.0 | TCP原生套接字管理 |
| libarchive | ≥ 3.0 | 目录打包与解压（tar.gz） |
| pthreads | 系统自带 | 多线程调度 |
| CMake | ≥ 3.10 | 项目构建工具 |

### 可选依赖（仅命令行模式，无需图形库）
| 工具 | 用途 |
|------|---------|
| `getopt_long` | 命令行参数解析（Glibc内置） |

---

## 安装教程
###  Debian/Ubuntu 系 Linux
```bash
# 安装编译依赖
sudo apt install -y build-essential cmake git \
    libsdl2-dev libwebsockets-dev libarchive-dev

# 拉取代码并编译
git clone <仓库地址> lanft
cd lanft
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# 可选：安装到系统全局
sudo cp lanft /usr/local/bin/
```

### Fedora/RHEL 系 Linux
```bash
sudo dnf install -y gcc cmake git \
    SDL2-devel libwebsockets-devel libarchive-devel
# 后续编译步骤与上文一致
```

### Arch Linux
```bash
sudo pacman -S --needed base-devel cmake git \
    sdl2 libwebsockets libarchive
# 后续编译步骤与上文一致
```

### 安卓 Termux
```bash
pkg update && pkg upgrade
pkg install cmake make clang git binutils \
    sdl2 libwebsockets libarchive termux-x11

# 编译程序
cd lanft && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# 启动图形界面（需安装Termux:X11应用）
termux-x11 :0 &
export DISPLAY=:0
./lanft
```

---

## 使用说明
### 图形界面模式（无附加参数）
```bash
./lanft
```
启动SDL2图形窗口，包含四个功能标签页：

| 标签页 | 功能说明 |
|-----|----------|
| **设备扫描** | 扫描局域网内所有运行lanft的设备，支持自定义端口；点击设备可自动填充IP地址 |
| **发送文件** | 选择文件/目录，切换文件/目录模式，填写**接收端IP**后开始传输；点击停止或按ESC取消 |
| **接收文件** | 选择文件保存路径，默认监听地址为`0.0.0.0`，开启监听等待接收；点击停止或按ESC取消 |
| **传输记录** | 展示历史传输列表，包含名称、时间、耗时、类型、端口、状态、进度、传输速度 |

**常规使用流程**
1. 接收端：进入接收页面 → 选择保存目录 → 开启监听（等待连接）
2. 发送端：进入发送页面 → 选择文件/目录 → 输入接收端IP → 开始传输
3. 两端同步显示传输进度，完成后自动存入传输记录

### 命令行模式（携带任意参数）
```bash
# TCP协议发送文件（默认端口9876）
lanft --mode=S --address=192.168.1.100 ./report.pdf

# UDP协议发送目录（自定义端口）
lanft --protocol=UDP --mode=S --address=10.0.0.5 -p 5555 /home/user/docs/

# 简写指令：发送文件
lanft -S -p 1234 ./video.mp4

# 接收文件（监听0.0.0.0:9876，保存至./downloads/目录）
lanft --mode=R ./downloads/

# 指定网卡与端口接收文件
lanft --mode=R --address=10.84.183.2 -p 5555 ./received/

# 查看帮助文档
lanft --help

# 查看版本信息
lanft --version

# 查看传输历史记录
lanft --history
```

### 参数对照表
| 短参数 | 长参数 | 默认值 | 说明 |
|-------|------|---------|-------------|
| `-h` | `--help` | — | 打印帮助信息并退出 |
| `-v` | `--version` | — | 打印版本信息并退出 |
| `-S` | `--mode=S` | *（必填）* | 简写，等同于`--mode=S`（发送模式） |
| `-R` | `--mode=R` | *（必填）*| 简写，等同于`--mode=R`（接收模式） |
| | `--mode=S\|R` | *（必填）* | 传输模式：发送 / 接收 |
| | `--protocol=TCP\|UDP` | TCP | 选择传输协议 |
| `-p` | `--port=端口号` | 9876 | 指定通信端口 |
| | `--address=IP地址` | `0.0.0.0` | 发送端填写接收IP；接收端填写监听IP |
| | `--history` | — | 输出传输记录并退出 |

---

## 工作原理
### 通信协议
1. **元数据握手（270字节）**：固定校验码`FT01`、协议类型、文件名、文件总大小、目录标识位
2. **应答报文（16字节）**：校验码 + 断点续传偏移量
3. **数据传输**：TCP直接传输原始字节；UDP采用分块报文+应答重传机制
4. 若标识为目录文件，传输内容为tar.gz压缩包，接收端自动解压

### 角色逻辑（类netcat）
```
接收端（服务端）: 监听端口 → 接受连接 → 接收元数据 → 接收文件数据
发送端（客户端）: 发起连接 → 发送元数据 → 接收应答 → 发送文件数据
```

### 设备扫描机制
- 通过`getifaddrs()`读取所有网卡，排除回环地址
- 提取各网卡对应的/24位局域网子网
- 启用32个线程，使用非阻塞TCP连接探测，超时时间800毫秒
- 支持局域网、无线网络、VPN、虚拟网桥等各类网络环境

### 断点续传原理
1. 发送端先发送文件总大小等元数据
2. 接收端检测本地是否存在同名未完成文件
3. 若文件存在且未传输完毕，返回当前已接收字节偏移量
4. 发送端定位到对应偏移位置，继续传输数据
5. TCP、UDP两种协议均支持该功能

---

## 命令行退出码
| 退出码 | 含义 |
|------|---------|
| 0 | 传输任务执行成功 |
| 1 | 执行出错（参数错误、连接失败、传输中断等） |

---

## 开源协议
MIT 许可证

## 星折线图
[![Star History Chart](https://api.star-history.com/chart?repos=Spinach5/lan-file-transfer&type=date&logscale&legend=top-left)](https://www.star-history.com/?repos=Spinach5%2Flan-file-transfer&type=date&logscale=&legend=top-left)