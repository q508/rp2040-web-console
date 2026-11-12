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

## 更新页面

`index.html` / `style.css` / `app.js` 都是从单文件版 `combo_webserial.html` 生成的。
改完单文件后执行：

```bash
python build_web.py
```

会把三个文件重新生成到 `web/`，上传覆盖即可，不用重启任何服务。
