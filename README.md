# RP2040 三合一 Web 控制台

PWM 信号源 + 双通道流式逻辑分析仪的网页上位机。

## 文件说明

| 文件 | 作用 |
|---|---|
| `index.html` | 页面结构 |
| `style.css` | 样式表 |
| `app.js` | 全部前端逻辑（无第三方库） |
| `start_console.bat` | 可选，双击用 Edge/Chrome 打开本页 |

三个文件必须放在同一目录，靠相对路径互相引用。**没有后端、没有数据库、不联网**，纯静态。

## 部署

把 `index.html` / `style.css` / `app.js` 一起上传到任意静态站点的同一目录即可：

```
/var/www/rp2040/index.html
/var/www/rp2040/style.css
/var/www/rp2040/app.js
```

访问 `https://your-domain/rp2040/`。Nginx / Apache / Caddy / IIS / 对象存储都行，不需要任何特殊配置，也不用改 MIME。

## ⚠️ 必须 HTTPS（否则串口用不了）

WebSerial 是「安全上下文」API，浏览器只在这些情况开放 `navigator.serial`：

| 访问方式 | WebSerial |
|---|---|
| `https://` 任意域名 | ✅ |
| `http://localhost` / `http://127.0.0.1` | ✅ |
| 本地双击 `index.html`（`file://`） | ✅ |
| `http://192.168.x.x` / `http://公网IP` | ❌ 页面能开，串口被禁用 |

纯 HTTP 部署到 IP 时，页面顶部会显示橙色警告。解决办法二选一：

- **配 HTTPS**（推荐）：给域名签个证书即可，静态站点几分钟的事；
- **临时白名单**（仅内网）：Chrome/Edge 打开 `chrome://flags/#unsafely-treat-insecure-origin-as-secure`，
  填入站点地址如 `http://192.168.1.10:8080`，设为 Enabled 后重启浏览器。

## 使用

1. 浏览器：**Chrome / Edge 89+**（Firefox、Safari 不支持 WebSerial）
2. 设备会枚举出 **3 个串口**：
   - 第 1 个 = 目标板串口桥
   - 第 2 个 = **LA 数据**（连 LA 选这个）
   - 第 3 个 = **PWM 命令**（连 PWM 选这个）
3. COM 号每台机器不同，**按顺序认，别按号码认**
4. 引脚定义、接线注意、USB 接口对照见页面第三个标签「引脚 / 接线」

## 升级固件

1. 网页「引脚 / 接线」页点 **进入升级模式**（免按 BOOTSEL，固件会软跳 ROM），或按住 BOOTSEL 插 USB；
2. 电脑出现 `RPI-RP2` 盘后，双击 `flash_uf2.bat`——它会扫描盘符、靠 `INFO_UF2.TXT` 认出 RPI-RP2，
   自动把 `firmware/` 下的 `.uf2` 复制进去（也可以手动拖）；
3. 板子自动重启，拔插一次 USB 让三个串口重新枚举。

`flash_uf2.bat` 与 `index.html` 放在同一目录即可，不依赖盘符。

## 固件与源码

- **烧录固件**：页面第三个标签「引脚 / 接线」顶部有下载按钮，文件在 `firmware/` 目录。
- **固件源码**：完整工程在 `firmware-src/`，含 Pico SDK 2.x 的 CMake 工程、FreeRTOS-Kernel、CMSIS-DAP 与全部驱动，
  编译方法见 `firmware-src/README.md`。

## 许可

本仓库代码 MIT，见 `LICENSE`。`firmware-src/` 里的 CMSIS-DAP（Apache-2.0）、FreeRTOS（MIT）
以及上游 debugprobe 基座沿用各自许可，详见 `firmware-src/README.md`。

## 发版流程

1. 更新 `firmware-src/VERSION`（如 `v1.1`）；
2. 重新编译固件，替换 `firmware/` 下的 `.uf2`；
3. 同步页面上三处：下载按钮 `href`、`.fw-meta`、`.fw-hash` 的 SHA-256；
4. `python build_web.py`；
5. 提交、打 tag、推送。

本地开发机上有 `release.sh` 把 1–5 串成一条命令：

```bash
./release.sh v1.1 "修复 xxx"     # 编译 + 归档 + 改页面 + 生成 + 提交 + tag + 推送
DRY_RUN=1 ./release.sh v1.1      # 只做前半段，不提交不推送
```

## 更新页面

`index.html` / `style.css` / `app.js` 都是从单文件版 `combo_webserial.html` 生成的。
改完单文件后执行：

```bash
python build_web.py
```

会把三个文件重新生成到 `web/`，上传覆盖即可，不用重启任何服务。
