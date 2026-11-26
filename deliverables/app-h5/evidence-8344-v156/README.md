# 834-4 残留清理 + v156 设置页同步 真机证据

设备：vivo V2046A / 1578472099005SJ，横屏 2376x1080。

## 校验点
- 01 登录页手机号/密码无预填（LOCAL_MODE/localResponse 桩已删除）。
- 02 输入服务端真实账号 13800000003 后进入「输入管理员密码」页，六格为空（无 111111 预填），证明登录走 /auth/login 服务端，非本地放行。
- 03 设置页只显示「网络状态：网络已连接」，无 SSID/信号行。
- 04 设备二维码弹窗含标题「设备二维码」、说明、设备号 2864 1907 5238 6419。

## SHA256
fe3be2d7fdf980cea188cac8db4e3d6f064f4d807f7b77135af67afb22853a81  01_login_no_prefill.png
da1481a400954881d5201ced22a26f0808032e11952a1ef02489b7063de0e669  02_login_server_admin_pin_empty.png
df514aa707c2a32e83a54f880b40b6bfd5f4d8bbb1fec0e0ce063facdafbea73  03_settings_network_two_state.png
edb2ddeb745a2511fe6165f8343bac973b54199f68a869735b47de1c1c01b3df  04_qr_title_desc_device.png