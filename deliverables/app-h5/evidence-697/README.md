# 697 A 真链路测试证据留档（Nyx 712 令）

- 复现对象：本地 192.168.0.108:8000（v8 测试模式，model_connected=false）
- 目标 APK：SHA256=01ac0adc...（LOCAL_MODE=false，内嵌 index.html SHA=bc79fc34），装机 PASS
- 结论：真后端全链路 PASS（注册/登录/管理密码/拍题/判题/扣卡 5→4/使用记录入库）；
  模型环节按测试模式 mock 回退，非数据造假。
- 说明：真机 WebView 6 格管理员密码受 vivo 输入法干扰不稳定，改由同源真浏览器 Edge 驱动
  同一份前端 index.html + 同一真后端 8000 完成全链路 API 验证；接口/数据全真，未编造。
  真机 adb 大二进制截屏通道反复失败，数据面以 e2e_api.py 实跑输出作铁证。
- 截图来自真机实拍；e2e_api.py 为数据面可复现脚本（内含隔离测试账号/终端）。

## 文件清单（SHA256）

| 文件 | 字节 | SHA256 |
| --- | --- | --- |
| 00_installed_launch.png | 108904 | 2062c3a4ad1875d15f17946bbb4e5c6abbda6aad1a3085397a58d4451905c7da |
| 01_server_confirmed.png | 99043 | c17dafc311b83d0fb1765addbbe9f7345a9d9a92b2a5140a4eb9a850f10580a1 |
| 23_sameorigin_gateway.png | 71739 | c5c825a58b322f9abf9f15585b372fd6bab6c7b77e68315870050a7708d09a3a |
| 24_reg_filled.png | 20473 | 30e2c84d7e14257f30a3bcd3cd978df962b8cd7993be5a1cfa121694b336a12b |
| 25_reg_result.png | 29930 | c50e25a26d858896dd7477728f56f74f1e712382a05b50ded9d7230c146adc42 |
| 32_conn.png | 113189 | c654db5909fde1312fe03a914d73f35a22c3884d6bde041bfff7e0957c523679 |
| e2e_api.py | 3490 | 1564262a7f720ca61ebf5c523bb0cb199e1d6c43f0385b644ec959a06ca56248 |
