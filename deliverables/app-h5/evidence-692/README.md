# evidence-692 · 注册免短信（msg692）

真机：vivo 1578472099005SJ，com.jianti.app 2.0.0，官方服务端 solve.mhq-ai.xyz:8000（v9.3.0）。

## 一、问题

浩爷定：短信验证暂时关闭，手机号+密码就能注册。实际在 App 里把「验证码」留空点注册，
弹「验证码格式不正确。」，注册失败。

服务端实测确认差异：

| 请求体 | 结果 |
|---|---|
| `{phone,password,admin_password,code:""}` | **400** `invalid_sms_code` 验证码格式不正确 |
| `{phone,password,admin_password}`（不传 code） | **201** 注册成功 |

App 旧代码固定传 `code:code`，验证码空着时传的是空串，所以必然 400。

## 二、改法

`deliverables/app-h5/index.html` 注册提交处：验证码为空时**整条 code 字段都不带**。

```js
var payload = { phone:phone, password:pwd, admin_password:adminPwd };
if (code) payload.code = code;
api.register(payload)
```

## 三、真机验证

| 文件 | 说明 |
|---|---|
| 01_before_empty_code_400.png | 修复前：验证码留空点注册，弹「验证码格式不正确。」 |
| 02_after_register_success_no_sms.png | 修复后：同样留空验证码，注册成功 |

修复后真机（未填验证码）注册 `13800000003` → 显示「注册成功」；
服务端复核该账号可登录：`/auth/login` 200，family 65478612，角色管理员，member_no 1。

## 四、账号处理说明

- `13800000002`：按 NYX msg692 指示保持原样放着，不再继续弄。
- `13800000003`：本次免短信注册验证用的测试号，密码 123456，管理员密码 123456。

## SHA256

```
9DED2ED6E9B3138B1D09FC477E5A6FD9CBF0020E5904786CFA2E270D756CDA78  01_before_empty_code_400.png
BE8715DE4F2D95AAB89790E43AC30C6262AE6AA7102D332572EF2376910F66C9  02_after_register_success_no_sms.png
```
