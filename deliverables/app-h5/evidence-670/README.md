# evidence-670 · 浩爷 App 登录账号（msg670）

真机：vivo 1578472099005SJ，com.jianti.app 2.0.0，服务端=官方 solve.mhq-ai.xyz:8000（v9.3.0）。
验证方式：卸载重装后的干净状态，从锁屏开始，全程只用 App UI 操作，未用脚本代填。

## 一、结果

账号已在生产服务端注册并真机登录成功，管理页正常进入。

| 项 | 值 |
|---|---|
| 登录方式 | 手机号 + 密码（App 第9屏登录，无独立用户名） |
| 手机号 | 13800000002 |
| 角色 | 管理员（member_no 1） |
| 家庭组 | 51356862 |
| 服务端 | 官方服务器 solve.mhq-ai.xyz:8000（App 设置里选「官方服务器」，地址已写死） |
| 管理员密码 | 6 位数字，注册时一并设置（与登录密码不同，进管理页要输） |

> 密码不写入本文件，避免凭据进仓库；由 TRA 当面/私回报给浩爷。

## 二、真机步骤与证据

1. `01_login_ok_to_adminpin.png`
   锁屏 → 左滑进 home → 左上管理入口 → 登录页，填手机号+密码点「登录」，
   登录成功直接进「输入管理员密码」页。

2. `02_management_page.png`
   输入六位管理员密码 → 进入管理页，顶部显示「账号信息 13800000002 管理员」，
   四个入口（卡管理／账户管理／终端管理／服务端管理）齐全。

3. `03_home_after_login.png`
   回首页：右上「服务器已连接」，解题卡「5 / 5 张」（登录前是 `--- / ---`），
   说明登录态已生效、卡余额从服务端拉到。

4. `04_terminals_bound.png`
   终端管理页：登录时服务端自动把本机 deviceId 绑到该家庭，
   终端1 = 2417 9686 8669 1468（本机，安卓）。

## 三、服务端侧核对（同一账号）

```
POST /auth/register {phone:13800000002,...}            -> 201  family_id 51356862
POST /auth/login    {phone,password} + X-Terminal-Id   -> 200  auto_bind=bound
POST /auth/admin-password/verify {admin_password}      -> 200  verified:true
GET  /family                                           -> 200  members[1]
GET  /usage                                            -> 200  温度/MEMO 正常
POST /cards/check                                      -> 200  balance 5
```

## 四、过程中发现的一件事（非本次改动引入）

首次用旧构建登录时，进管理页会被 401 踢回登录页并提示「登录已失效，请重新登录」；
重装同一份代码后从干净状态走完全程无法复现（01→02 一次通过）。
定位时给 WebView 加了临时 HTTP 状态日志，整轮登录+管理页加载期间**没有任何非 2xx 响应**，
说明服务端与本轮链路都是通的；判断是旧安装残留的会话/绑定状态导致，已在重装后消除。
临时诊断代码已撤销，交付的 index.html / MainActivity.java 与 dev 提交一致。

## SHA256

```
3F639C0B102CF24797A991D8AD1368D1A1E5CFEE2C064A1C7980ED584AA1D5F7  01_login_ok_to_adminpin.png
CCB1061F91C571080DDC2FD1CECFEB10ADFB20E4F44F6AD30483C3F4AB433933  02_management_page.png
BC2F72702C8BC96058C91EED32A5F7E1126BFE8131F39A5046AFD9C02815AC38  03_home_after_login.png
AAD2991AE211946FF5DD01032B978BBF164802A44B65B5809A46E757695C8FE1  04_terminals_bound.png
```
