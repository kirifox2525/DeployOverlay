# kiri System Deploy Overlay

自用系统部署时显示的悬浮窗

## Sysprep specialize 调用

悬浮窗需要持续显示，因此启动命令必须立即返回；不直接把 EXE 作为阻塞式 `RunSynchronous` 命令。可用：

```xml
<RunSynchronousCommand wcm:action="add">
  <Order>1</Order>
  <Description>Overlay</Description>
  <Path>cmd /c start "" "<PATH>\DeployOverlay.exe"</Path>
</RunSynchronousCommand>
```

## 测试

- 可用 `--lang=zh-CN`、`--lang=zh-TW`、`--lang=en-US` 临时覆盖语言。
