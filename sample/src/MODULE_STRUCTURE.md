# TauriCPP Sample 应用模块化结构

## 目录结构

```
sample/src/
├── main.cpp                    # 程序入口，只包含初始化逻辑
├── Core/                       # 核心工具层（不依赖 Webview/UI）
│   ├── StringUtils.h          # 字符串转换工具
│   ├── FileManager.h          # 文件操作工具
│   ├── HashUtils.h            # 哈希计算工具
│   ├── HttpClient.h           # HTTP 客户端
│   └── ConfigManager.h        # 配置管理
├── Bridge/                     # 桥接层（前后端通信）
│   └── WebviewBridge.h        # Webview 桥接接口
├── Models/                     # 数据模型层
│   ├── DownloadTypes.h        # 下载相关类型
│   └── MavenCoord.h           # Maven 坐标
└── Services/                   # 业务服务层
    ├── AccountService.h        # 账号服务
    ├── VersionService.h        # 版本服务
    ├── DownloadService.h       # 下载服务
    ├── JavaService.h           # Java 服务
    ├── GameLaunchService.h     # 游戏启动服务
    ├── ManifestService.h       # 版本清单同步服务
    └── MavenUtils.h            # Maven 工具类
```

## 依赖关系

```
┌─────────────────────────────────────────────────────────────┐
│                        main.cpp                             │
│                    (程序入口 & 初始化)                        │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                       Services 层                           │
│  AccountService │ VersionService │ DownloadService │ ...   │
└─────────────────────────────────────────────────────────────┘
                              │
              ┌───────────────┼───────────────┐
              ▼               ▼               ▼
┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐
│   Bridge 层    │ │    Core 层     │ │   Models 层    │
│  WebviewBridge │ │ ConfigManager  │ │ DownloadTypes  │
│                │ │ HttpClient     │ │ MavenCoord     │
│                │ │ FileManager    │ │                │
│                │ │ HashUtils      │ │                │
│                │ │ StringUtils    │ │                │
└─────────────────┘ └─────────────────┘ └─────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                      tauricpp 库                            │
│           (WebView2 窗口管理 & 前后端通信)                    │
└─────────────────────────────────────────────────────────────┘
```

## 设计原则

1. **Core 层完全不依赖 Webview 或 UI 代码**
   - 只通过 `WebviewBridge` 与前端交互
   - 包含纯工具类和配置管理

2. **Services 层封装所有业务逻辑**
   - 每个服务负责一个功能领域
   - 通过 `Bridge::WebviewBridge` 注册命令

3. **Models 层定义数据结构**
   - 不包含业务逻辑
   - 用于层间数据传递

4. **Bridge 层封装通信接口**
   - 隔离 tauricpp 库的具体实现
   - 提供统一的命令注册和事件发送接口

## 代码行数对比

| 文件 | 重构前 | 重构后 |
|------|--------|--------|
| main.cpp | 3141 行 | 75 行 |
| 总计 | 3141 行 | ~1500 行（分散到各模块） |

## 扩展指南

### 添加新的服务

1. 在 `Services/` 目录创建新的 `.h` 文件
2. 继承或注入 `Bridge::WebviewBridge`
3. 在构造函数中调用 `RegisterCommands()` 注册命令
4. 在 `main.cpp` 中创建服务实例

### 添加新的数据模型

1. 在 `Models/` 目录创建新的 `.h` 文件
2. 定义纯数据结构，不包含业务逻辑
