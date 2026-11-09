# 第 48 批 · 当前版本清单（逐文件位置 / SHA-256 / 版本号）

- 生成：2026-11-09 23:16（北京时间），WBC，全只读复算。
- 口径：本清单是**当前版本基线的可复算快照**，不是全量 30 GB 盘点；完整逐文件证据见 `12_EVIDENCE_LOCAL_BASELINE.json`。
- 哈希算法：SHA-256（文件原始字节，非 Git blob SHA-1）；单文件 >20 MB 标 `SKIP_TOO_LARGE` 不强行入表。
- 「推／未推」列的最终结论见 `05_PUSH_RECEIPT.md`（推送完成后回填）。

## 一、点名基线（任务文档第 13 行，逐件复算对上）

| 项 | 版本号 | 路径 | 字节 | SHA-256 | 对照 |
| --- | --- | --- | ---: | --- | --- |
| 需求分析 | v1.1 | `F:/AIsoft/data/简题2.0/codex/简题App2.0_需求分析_20261109.md` | 11,834 | `dfe62d5c8ec4fc6e1299ada4c4e9ce4cf69673b3bba8458c142068e0cf7bf02d` | dfe62d5c ✅ |
| UI/UE 交互规范 | v1.1 | `F:/AIsoft/data/简题2.0/codex/简题App2.0_UIUE交互规范_20261109.md` | 12,060 | `bea7b3879b73efde03337f05e1dd7617dc66d5af049b286a26f8ef4a1052f88f` | bea7b387 ✅ |
| PRD | v1.0 | `F:/AIsoft/data/简题2.0/WBC/deliverables/20261109/batch42/docs/PRD_V1.0.md` | 6,976 | `50e53beceeaa4ba46ffab69aa80b05fa047df865c3ac49efc0f7629bfb9bf285` | 50e53bec ✅ |
| 网关（含入口修复＋8MB 修复） | v3.23 | `F:/AI/jianti/gateway_v3.23/gateway.py` | 118,209 | `6e615702169e9ed027a804e26c0e80cfd5cd368ca2b69f20d3924d3ea26f1167` | 6e615702 ✅ |
| Android App 基线 APK | b25 线 | `F:/b25src/app-debug.apk` | 6,501,555 | `ea5d1f1412f34125262398f32ad4a04c3976649fab352b84fef66aa459660a19` | ea5d1f14 ✅ |
| HarmonyOS 壳 Index.ets | 2.0 线 | `F:/AI/jianti/app/shell/entry/src/main/ets/pages/Index.ets` | 75,976 | `d4ccc2fed6dde74c86da248bb8fdc6817dc7793309eee5e45febf8cc5e31b746` | d4ccc2fe ✅ |
| 第 48 批任务文档 | 48 批 | `F:/AIsoft/data/简题2.0/简题2.0_开发任务_第48批_WBC_GitHub版本管理.md` | 2,889 | `2c56d727c37d723c9bfdd0bb82b2aaeff641b33309074003e45429b179d73190` | 2c56d727 ✅ |

> 5 份产品／代码真件 + 任务文档全部与任务文档、Nyx 简报给出的 SHA 前缀一致；`app-debug.apk` 与鸿蒙壳为本批补录，任务文档未点名但属当前版本基线。

## 二、分组逐文件清单

### 网关 v3.23 全目录

- 根目录：`F:/AI/jianti/gateway_v3.23`；文件数 95；已计字节 4,557,733；版本标识：**v3.23**
- 下表路径为相对该根目录的路径（正斜杠）。

| 相对路径 | 字节 | SHA-256 | 修改时间 |
| --- | ---: | --- | --- |
| `_dbg_err.log` | 2,056 | `1b15dcc389523fb3376dc35fb619848b3d9dbf731ad8579f0659eb67d84b8e6c` | 2026-11-07 20:42:11 |
| `_dbg_out.log` | 1,263 | `1edf46069132eb9c6bd35c541ff12acb3311e2be9cf7d193c38bfc6c1c449949` | 2026-11-07 20:42:11 |
| `admin_routes.py` | 5,703 | `f07a690b538d32e9dc748af54b1b13314d719c35254e4614cf599d99b4895575` | 2026-10-29 23:35:09 |
| `api_prio_flags.jsonl` | 2,424 | `1b99acb3cd54fe9d2bbf25e890365d6d197a6756621e4015ddcf1ae3791c99f5` | 2026-11-09 05:01:12 |
| `autorun.log` | 838 | `1823db673ea5786aaac6fb8d3511ddc2696464f41c6c0fb02f9073a58b65d466` | 2026-11-08 05:10:16 |
| `autorun.pyw` | 2,216 | `85e30f3f2649cda8c1c5656445a3d08f836185c9f50735f85dabf50041cbea95` | 2026-10-29 20:56:29 |
| `gateway.py` | 118,209 | `6e615702169e9ed027a804e26c0e80cfd5cd368ca2b69f20d3924d3ea26f1167` | 2026-11-08 20:22:51 |
| `gateway_autorun.log` | 6,467 | `26ef40b80b94bed9fd8e1bef1b08b1f009e21ac4cfddcc39fd993689eab5b500` | 2026-11-08 05:51:28 |
| `gateway_manual_err.log` | 2,061 | `b318fdab65ea9b7347cdbcc4ffd40e656b3d6e5fea97d70a843885c3f426e9b5` | 2026-11-07 22:06:00 |
| `gateway_manual_out.log` | 2,817 | `dba4ee0ce8c276df41e8e9363358078ca227214ec07e4494eb3506cc8347a441` | 2026-11-07 22:06:00 |
| `gw_real_err.log` | 6,035 | `f1aab0eb4fd79fc90cf3a2da3321b825aa02c645fee5ea33c347e753b2a68c4d` | 2026-11-08 20:22:58 |
| `gw_real_out.log` | 10,184 | `e36b1e91fe599ba88ae1538cfc3a8226def1b8b1aeae06912f4d2aab4d56340e` | 2026-11-08 20:22:58 |
| `leak_flags.jsonl` | 2,302 | `5a6f515bee82be4c332c9cd5241f82130c8d620af1408f46373430c6cdca49a5` | 2026-11-08 20:23:51 |
| `qw_bridge.py` | 55,652 | `9a3deced95b6bd49d348a06e42e1cb92da1d243a5b2c43f97b3dfc1bf7b577ab` | 2026-10-30 08:35:45 |
| `qw_bridge_autorun.log` | 112 | `711aa81c630db4dae05b6bf0ce045e46f5bce51125cfe418ca55ec6402af1b35` | 2026-11-08 05:10:16 |
| `raw_texts.jsonl` | 13,247 | `29e2be72e7fbd7b961a8632d531a7b74d042687cd7681443535ba985d56bdea3` | 2026-11-09 05:01:12 |
| `requirements.txt` | 29 | `6dea1d21c765b980fa4ae726678714623d0d31d1e1518ef422a31e40bd94639b` | 2026-10-30 08:05:23 |
| `rollback-v3.20-rotate.zip` | 77,214 | `4abd901b5119c56a5390882146652257fc348f871282227ee1c291310e73c4a1` | 2026-10-26 02:35:06 |
| `run_gateway.ps1` | 1,132 | `2297f205c399ddeaac899dafa3a2443637d62ab863f8b0f47d4757fdcf0863c6` | 2026-10-30 08:35:53 |
| `smoke_1x1.jpg` | 160 | `96e43e69bd61a92505b9e658edb066c4663453706575bcfdebfa37906c620079` | 2026-11-07 20:43:50 |
| `split_records.jsonl` | 2,141 | `ada47dae3f4217eb237f6df6a8dbf9703301cdc50df29a85b93b13e4c05571b1` | 2026-11-09 05:01:12 |
| `start_gw_wbapi.bat` | 205 | `9cf175d5d2adf6fa7324a2692a8e00a8c0acd428d2da91f5e1c46176eaafa997` | 2026-10-30 08:35:57 |
| `test_q_real.jpg` | 27,766 | `4053d6d33e3325bf93105c9b5ad2f96b93a3141c5dd00284e33a9a03c829c5fc` | 2026-11-07 22:11:49 |
| `watchdog.py` | 5,714 | `e46f041bfdd5f21f5783da17e4e682892a983033f68126fd710c40cd54c8f3aa` | 2026-10-30 08:35:49 |
| `wb_model_trace.jsonl` | 693 | `dafbd10929bbd7493d7a1b27a37492edde846112f0f9315335ab7d947fc369ea` | 2026-11-08 05:11:09 |
| `uploads/jt_1794055874.jpg` | 160 | `96e43e69bd61a92505b9e658edb066c4663453706575bcfdebfa37906c620079` | 2026-11-07 20:51:14 |
| `uploads/jt_1794055951.jpg` | 160 | `96e43e69bd61a92505b9e658edb066c4663453706575bcfdebfa37906c620079` | 2026-11-07 20:52:31 |
| `uploads/jt_1794055976.jpg` | 160 | `96e43e69bd61a92505b9e658edb066c4663453706575bcfdebfa37906c620079` | 2026-11-07 20:52:56 |
| `uploads/jt_1794056987.jpg` | 160 | `96e43e69bd61a92505b9e658edb066c4663453706575bcfdebfa37906c620079` | 2026-11-07 21:09:47 |
| `uploads/jt_1794060249.jpg` | 160 | `96e43e69bd61a92505b9e658edb066c4663453706575bcfdebfa37906c620079` | 2026-11-07 22:04:09 |
| `uploads/jt_1794060487.jpg` | 160 | `96e43e69bd61a92505b9e658edb066c4663453706575bcfdebfa37906c620079` | 2026-11-07 22:08:07 |
| `uploads/jt_1794060670.jpg` | 160 | `96e43e69bd61a92505b9e658edb066c4663453706575bcfdebfa37906c620079` | 2026-11-07 22:11:10 |
| `uploads/jt_1794060670.json` | 705 | `cef76068fdc00b7099157db696628b2794c9caf8f74195625366928d442279b0` | 2026-11-07 22:11:16 |
| `uploads/jt_1794060670_cards.json` | 722 | `20eeec48a041a66d4b5aed5704de8f42307c27fc3a6736830d33bf392d15dd02` | 2026-11-07 22:11:16 |
| `uploads/jt_1794060709.jpg` | 27,766 | `4053d6d33e3325bf93105c9b5ad2f96b93a3141c5dd00284e33a9a03c829c5fc` | 2026-11-07 22:11:49 |
| `uploads/jt_1794060709.json` | 349 | `d97c80e79f8c7776cb72fbb5313cae3d18b550c943cd31193db07b6b93ee9420` | 2026-11-07 22:11:52 |
| `uploads/jt_1794060709_cards.json` | 408 | `e4ec7ccb6468f5c2a715591f257f3c64bd9b358e46af9f928ed2fdadadf0e6b8` | 2026-11-07 22:11:52 |
| `uploads/jt_1794060760.jpg` | 27,766 | `4053d6d33e3325bf93105c9b5ad2f96b93a3141c5dd00284e33a9a03c829c5fc` | 2026-11-07 22:12:40 |
| `uploads/jt_1794060760.json` | 423 | `bd4f1aa3c12e26773d78651f6f728fc3392c297db8f23dd7400034df5871ae81` | 2026-11-07 22:12:43 |
| `uploads/jt_1794060760_cards.json` | 470 | `ba118a6ce32e4723bcdaa59dcd7e6b46e4620f3e2c47bb43a8ee6ef4deddf1b2` | 2026-11-07 22:12:43 |
| `uploads/jt_1794060795.jpg` | 27,766 | `4053d6d33e3325bf93105c9b5ad2f96b93a3141c5dd00284e33a9a03c829c5fc` | 2026-11-07 22:13:15 |
| `uploads/jt_1794060795.json` | 455 | `a7007b629a61289c27721b5f7372f561f98dbf5d672b7ae10c77961cab1a4816` | 2026-11-07 22:13:18 |
| `uploads/jt_1794060795_cards.json` | 503 | `2ad91aa7eb9b30da0d1395fdc3c48b4a1bd2177989ddf5676209cbeecaed474e` | 2026-11-07 22:13:18 |
| `uploads/jt_1794068142.jpg` | 27,766 | `4053d6d33e3325bf93105c9b5ad2f96b93a3141c5dd00284e33a9a03c829c5fc` | 2026-11-08 00:15:42 |
| `uploads/jt_1794068142.json` | 329 | `f8f9a65f4db2e79320c15ebc6965bcea0db430d026a5ee01762803db935da0e6` | 2026-11-08 00:15:45 |
| `uploads/jt_1794068142_cards.json` | 384 | `a21af764d1790507951464aa1e175db456b9bad95452cfc59d05f9564acc3081` | 2026-11-08 00:15:45 |
| `uploads/jt_1794068732.jpg` | 27,766 | `4053d6d33e3325bf93105c9b5ad2f96b93a3141c5dd00284e33a9a03c829c5fc` | 2026-11-08 00:25:32 |
| `uploads/jt_1794068732.json` | 417 | `ecfb50b4267fec1f984602a161687a9080933033ce5747695432c587a7a2038b` | 2026-11-08 00:25:42 |
| `uploads/jt_1794068732_cards.json` | 456 | `9ea1bbc8b4f3c1f03c72a54221fef6d4908ed65851285b6bc8cbfedb0afa1382` | 2026-11-08 00:25:42 |
| `uploads/jt_1794071333.jpg` | 27,766 | `4053d6d33e3325bf93105c9b5ad2f96b93a3141c5dd00284e33a9a03c829c5fc` | 2026-11-08 01:08:53 |
| `uploads/jt_1794071333.json` | 454 | `5d09d135c6352882c6922f4831ce090548c8b492c59e235f10358f6cb0f429e0` | 2026-11-08 01:08:55 |
| `uploads/jt_1794071333_cards.json` | 487 | `cd6b89e6183c86bce6e6162a56f13977c08754716388e4e9a9f4febe0b06c4ab` | 2026-11-08 01:08:55 |
| `uploads/jt_1794073111.jpg` | 20,967 | `4546907b731a30ef85b485731ab98056b33eb8064c8665c79d33f6d4a3758cba` | 2026-11-08 01:38:31 |
| `uploads/jt_1794073111.json` | 236 | `2fb7f52386c51d3454342cc0dc46afb4e2c7d877791912c8388a9aaab1b3477b` | 2026-11-08 01:38:42 |
| `uploads/jt_1794073111_cards.json` | 297 | `8730cb118a7a1a9d40b6372b69fe91c356c07c14aa714a64e6de7666478cdcfa` | 2026-11-08 01:38:42 |
| `uploads/jt_1794073133.jpg` | 20,967 | `4546907b731a30ef85b485731ab98056b33eb8064c8665c79d33f6d4a3758cba` | 2026-11-08 01:38:53 |
| `uploads/jt_1794073133.json` | 288 | `5f24b46b265607f2e3466609f6b3bd09432870bd90a0b5c399cb70f49aa054a7` | 2026-11-08 01:39:00 |
| `uploads/jt_1794073133_cards.json` | 349 | `55f79cc00e56d6d49a577bda12546ca89085b338b0d5db0ef3f9ae89038b8b35` | 2026-11-08 01:39:00 |
| `uploads/jt_1794073170.jpg` | 20,967 | `4546907b731a30ef85b485731ab98056b33eb8064c8665c79d33f6d4a3758cba` | 2026-11-08 01:39:30 |
| `uploads/jt_1794073170.json` | 185 | `176106852d90e172ca48d9b99b072d6fea7577ffa572dc2a0f1348c17308cf9d` | 2026-11-08 01:39:32 |
| `uploads/jt_1794073170_cards.json` | 246 | `f3e9890f99d5a7898ff1327bcbea940534d3ad017e7fc30a68630b14857995fe` | 2026-11-08 01:39:32 |
| `uploads/jt_1794073565.jpg` | 27,707 | `bf66e46546c640abeadeb25267e46bef966045facc01878404e5397f22093849` | 2026-11-08 01:46:05 |
| `uploads/jt_1794073565.json` | 430 | `0dc72b3b8e0c773f1d07b7f82f28446fb3780bce36014ee0adb5843146e20e8d` | 2026-11-08 01:46:14 |
| `uploads/jt_1794073565_cards.json` | 435 | `6cdab2ec34eb9605e0c3400eb5e03da9e0c78e9251998251a11734bbc1167e1c` | 2026-11-08 01:46:14 |
| `uploads/jt_1794073594.jpg` | 27,788 | `2f49305e228ff9e144282603e84f8615e13463adfc43a5c77e833fa7dadf75c7` | 2026-11-08 01:46:34 |
| `uploads/jt_1794073594.json` | 576 | `daec3aaa5237cce3c64b33ec1efa1be6607e2408eeaaa36ac4ea8d6b6fc4b080` | 2026-11-08 01:46:39 |
| `uploads/jt_1794073594_cards.json` | 584 | `8e93baf471b27a70d207cbd7c6e87bbd6d746d9c40067eec966019af5187328b` | 2026-11-08 01:46:39 |
| `uploads/jt_1794073843.jpg` | 27,788 | `2f49305e228ff9e144282603e84f8615e13463adfc43a5c77e833fa7dadf75c7` | 2026-11-08 01:50:43 |
| `uploads/jt_1794073843.json` | 464 | `124307970138792612d6e1d480bc3fa976f04fa9f4c90cc41147dcb3f7ed0531` | 2026-11-08 01:50:53 |
| `uploads/jt_1794073843_cards.json` | 497 | `78c8bcce8769a0c1b8caeceb3ea8b7303f285f36c33bbe76b6ddbb7d12c1bc7f` | 2026-11-08 01:50:53 |
| `uploads/jt_1794073938.jpg` | 27,788 | `2f49305e228ff9e144282603e84f8615e13463adfc43a5c77e833fa7dadf75c7` | 2026-11-08 01:52:18 |
| `uploads/jt_1794073938.json` | 522 | `feaaf88b477c68c8a1b1d0a516ed954717966ababbb7418b3d299a4690e6e577` | 2026-11-08 01:52:23 |
| `uploads/jt_1794073938_cards.json` | 529 | `cd4875b9f5e5b298b2fef8cf8ff75ab0e27cbc5eb8b5fb657bcacfec5cd26244` | 2026-11-08 01:52:23 |
| `uploads/jt_1794085855.jpg` | 27,610 | `6d4c28fb226e5f3baee25e79f5490bfaefc6b792b2f743c7ba253fbae68ae6e7` | 2026-11-08 05:10:55 |
| `uploads/jt_1794085855.json` | 641 | `d9c59273521102664e11a69cab9185dda0a0025b11f7bfcb982a1047be02686e` | 2026-11-08 05:11:09 |
| `uploads/jt_1794085855_cards.json` | 702 | `7e117a94e68dc353ff3e8b0b5289df43914a996b9c6515f3a38eb1448cb80827` | 2026-11-08 05:11:09 |
| `uploads/jt_1794134317.jpg` | 27,766 | `4053d6d33e3325bf93105c9b5ad2f96b93a3141c5dd00284e33a9a03c829c5fc` | 2026-11-08 18:38:37 |
| `uploads/jt_1794134317.json` | 412 | `30c7f4ed56d24c5d2806fabc5f069f5441c78364b1e946155df284c2719a85d4` | 2026-11-08 18:38:43 |
| `uploads/jt_1794134317_cards.json` | 453 | `00acf2ae02a2fdca512e4883c33c9103c9cec3574b4714e62f263921ae973405` | 2026-11-08 18:38:43 |
| `uploads/jt_1794134323.jpg` | 27,766 | `4053d6d33e3325bf93105c9b5ad2f96b93a3141c5dd00284e33a9a03c829c5fc` | 2026-11-08 18:38:43 |
| `uploads/jt_1794134323.json` | 472 | `9ff0333c6ef45e7039b49bef48a0e805f71bd70fa84ce50c47ba39dc7fcdea90` | 2026-11-08 18:38:47 |
| `uploads/jt_1794134323_cards.json` | 519 | `2ac505351435bac32a4ecf2ea52cf40d2e449d8345130b8f681542666874e1ca` | 2026-11-08 18:38:47 |
| `uploads/jt_1794134327.json` | 381 | `d0eb61a480d4097b056182abb882dc6595fe3ca726729ce1f76c439a4bc4a164` | 2026-11-08 18:39:42 |
| `uploads/jt_1794134327.txt` | 75 | `027ed7241149896e2a9935cf0a5bd2790b2dbb732ea1862b03797fb2c0af3d56` | 2026-11-08 18:38:47 |
| `uploads/jt_1794134327_cards.json` | 442 | `2cac6efe1edac238810c0618e24cae797006397a2e8992b9c08d0ea0d5bda356` | 2026-11-08 18:39:42 |
| `uploads/jt_1794134382.jpg` | 27,766 | `4053d6d33e3325bf93105c9b5ad2f96b93a3141c5dd00284e33a9a03c829c5fc` | 2026-11-08 18:39:42 |
| `uploads/jt_1794134382.json` | 459 | `6ebe77e475ed07bc4bf40b89bdd046efe64a9ae7ab8c683bb3c42d1548c5c095` | 2026-11-08 18:39:45 |
| `uploads/jt_1794134382_cards.json` | 505 | `5288ae6fda7323ef671f0eb7c19c2228a6e097aeb4b9a149b04112dae9379094` | 2026-11-08 18:39:45 |
| `uploads/jt_1794140619.jpg` | 3,738,194 | `791ee40022659930c825bbd47de7695d4e20c0d382007ef958f2c59bf0313904` | 2026-11-08 20:23:39 |
| `uploads/jt_1794140619.json` | 1,006 | `cd9444da7ae344c658683e586314c7bd0fc2f5d1a772bec7cdfef35abe0efb38` | 2026-11-08 20:23:51 |
| `uploads/jt_1794140619_cards.json` | 989 | `f39d6ef96eea22d137fd3161d02386c1a15f39b80ca1bc64ddbd67ba4820f86b` | 2026-11-08 20:23:51 |
| `uploads/jt_1794171624.json` | 488 | `4b7e216381ba9b1c8cb48cc411ce1a8de9fdf0a425d7a0f6b27c6420b9825ba0` | 2026-11-09 05:01:12 |
| `uploads/jt_1794171624.txt` | 5 | `b9ab5b7f58369cf5558c951a9e13db37fd75fbab7c6b276f42b0abfaef5b96ab` | 2026-11-09 05:00:24 |
| `uploads/jt_1794171624_cards.json` | 549 | `546ce94aca31efd3cf548d91aa953c3468065e7dc3add1b1181f7015725c8223` | 2026-11-09 05:01:12 |
| `uploads/jt_1794171712.txt` | 5 | `b9ab5b7f58369cf5558c951a9e13db37fd75fbab7c6b276f42b0abfaef5b96ab` | 2026-11-09 05:01:52 |

### 隔离环境网关副本

- 根目录：`F:/AIsoft/data/简题2.0/DSH/_work/isolated_env/gateway`；文件数 30；已计字节 349,708；版本标识：**v3.23（副本）**
- 下表路径为相对该根目录的路径（正斜杠）。

| 相对路径 | 字节 | SHA-256 | 修改时间 |
| --- | ---: | --- | --- |
| `_dbg_err.log` | 2,056 | `1b15dcc389523fb3376dc35fb619848b3d9dbf731ad8579f0659eb67d84b8e6c` | 2026-11-07 20:42:11 |
| `_dbg_out.log` | 1,263 | `1edf46069132eb9c6bd35c541ff12acb3311e2be9cf7d193c38bfc6c1c449949` | 2026-11-07 20:42:11 |
| `admin_routes.py` | 5,703 | `f07a690b538d32e9dc748af54b1b13314d719c35254e4614cf599d99b4895575` | 2026-10-29 23:35:09 |
| `api_prio_flags.jsonl` | 2,313 | `3e77105527a1458ceae9aa5b7e4da873985053ea0d95c1da5fdc7153f6bcdf67` | 2026-11-08 20:23:51 |
| `autorun.log` | 838 | `1823db673ea5786aaac6fb8d3511ddc2696464f41c6c0fb02f9073a58b65d466` | 2026-11-08 05:10:16 |
| `autorun.pyw` | 2,216 | `85e30f3f2649cda8c1c5656445a3d08f836185c9f50735f85dabf50041cbea95` | 2026-10-29 20:56:29 |
| `cards_ledger.jsonl` | 1,676 | `4df95de9647a31a7bcf144535e3476661bfb35f4c3b45d7990716ecf0ae14a34` | 2026-10-28 08:45:20 |
| `cards_state.json` | 2,193 | `7941c81f2592fb44be3643cf19b0c92e2e0b8125f7ac29030268be5497802795` | 2026-10-28 19:18:23 |
| `gateway.py` | 118,209 | `6e615702169e9ed027a804e26c0e80cfd5cd368ca2b69f20d3924d3ea26f1167` | 2026-11-08 20:22:51 |
| `gateway_autorun.log` | 6,467 | `26ef40b80b94bed9fd8e1bef1b08b1f009e21ac4cfddcc39fd993689eab5b500` | 2026-11-08 05:51:28 |
| `gateway_manual_err.log` | 2,061 | `b318fdab65ea9b7347cdbcc4ffd40e656b3d6e5fea97d70a843885c3f426e9b5` | 2026-11-07 22:06:00 |
| `gateway_manual_out.log` | 2,817 | `dba4ee0ce8c276df41e8e9363358078ca227214ec07e4494eb3506cc8347a441` | 2026-11-07 22:06:00 |
| `gw_real_err.log` | 6,035 | `f1aab0eb4fd79fc90cf3a2da3321b825aa02c645fee5ea33c347e753b2a68c4d` | 2026-11-08 20:22:58 |
| `gw_real_out.log` | 10,184 | `e36b1e91fe599ba88ae1538cfc3a8226def1b8b1aeae06912f4d2aab4d56340e` | 2026-11-08 20:22:58 |
| `leak_flags.jsonl` | 2,302 | `5a6f515bee82be4c332c9cd5241f82130c8d620af1408f46373430c6cdca49a5` | 2026-11-08 20:23:51 |
| `qw_bridge.py` | 55,652 | `9a3deced95b6bd49d348a06e42e1cb92da1d243a5b2c43f97b3dfc1bf7b577ab` | 2026-10-30 08:35:45 |
| `qw_bridge_autorun.log` | 112 | `711aa81c630db4dae05b6bf0ce045e46f5bce51125cfe418ca55ec6402af1b35` | 2026-11-08 05:10:16 |
| `raw_texts.jsonl` | 12,627 | `27d6c79944de74a3ec62375434fd262aadbf591f8eee9b7e8b3c8d0ce28a5f35` | 2026-11-08 20:23:51 |
| `requirements.txt` | 29 | `6dea1d21c765b980fa4ae726678714623d0d31d1e1518ef422a31e40bd94639b` | 2026-10-30 08:05:23 |
| `rollback-v3.20-rotate.zip` | 77,214 | `4abd901b5119c56a5390882146652257fc348f871282227ee1c291310e73c4a1` | 2026-10-26 02:35:06 |
| `run_gateway.ps1` | 1,132 | `2297f205c399ddeaac899dafa3a2443637d62ab863f8b0f47d4757fdcf0863c6` | 2026-10-30 08:35:53 |
| `smoke_1x1.jpg` | 160 | `96e43e69bd61a92505b9e658edb066c4663453706575bcfdebfa37906c620079` | 2026-11-07 20:43:50 |
| `split_records.jsonl` | 2,050 | `a37f6f10c40b50743c6231f6762cdaf50579d3c6cfc83a5319ba15568dc7595c` | 2026-11-08 20:23:51 |
| `start_gw_wbapi.bat` | 205 | `9cf175d5d2adf6fa7324a2692a8e00a8c0acd428d2da91f5e1c46176eaafa997` | 2026-10-30 08:35:57 |
| `test_q_real.jpg` | 27,766 | `4053d6d33e3325bf93105c9b5ad2f96b93a3141c5dd00284e33a9a03c829c5fc` | 2026-11-07 22:11:49 |
| `watchdog.py` | 5,714 | `e46f041bfdd5f21f5783da17e4e682892a983033f68126fd710c40cd54c8f3aa` | 2026-10-30 08:35:49 |
| `wb_model_trace.jsonl` | 693 | `dafbd10929bbd7493d7a1b27a37492edde846112f0f9315335ab7d947fc369ea` | 2026-11-08 05:11:09 |
| `uploads/jt_1794163927.txt` | 7 | `5afb1ca7073145c715533774bb86375ff25272d967eb030d65bb1d4388794818` | 2026-11-09 02:52:07 |
| `uploads/jt_1794163932.txt` | 7 | `5afb1ca7073145c715533774bb86375ff25272d967eb030d65bb1d4388794818` | 2026-11-09 02:52:12 |
| `uploads/jt_1794163966.txt` | 7 | `5afb1ca7073145c715533774bb86375ff25272d967eb030d65bb1d4388794818` | 2026-11-09 02:52:46 |

### Android/Kotlin 源码

- 根目录：`F:/b25src/app/src`；文件数 24；已计字节 70,156；版本标识：**b25 线**
- 下表路径为相对该根目录的路径（正斜杠）。

| 相对路径 | 字节 | SHA-256 | 修改时间 |
| --- | ---: | --- | --- |
| `main/AndroidManifest.xml` | 2,271 | `a5d6bdb48c89ec11ff90fc8300e50b816935d925266e9b44dc0df6b9b195d8a5` | 2026-11-09 22:57:25 |
| `main/java/com/jianti/app/Session.kt` | 3,111 | `0022a375cd243d79a6fc22b742a89cae841482a573b2fe3f5bf081a072b37e88` | 2026-11-09 20:04:32 |
| `main/java/com/jianti/app/api/ApiClient.kt` | 2,198 | `2de5b349c8dcf2646e29ab3cbab7b68e1dbfcc2b1760dc9b04b57dddc60d9fc5` | 2026-11-09 05:55:11 |
| `main/java/com/jianti/app/api/ApiService.kt` | 1,924 | `892ea9138cc0f774708f25431be1de915d15f1a7377700ea653ea69e5fdce765` | 2026-11-09 02:28:36 |
| `main/java/com/jianti/app/ui/BindActivity.kt` | 3,377 | `2459fa9c48426160633981efa46f4d218b31b7b6d29f71deaf3dbec0b0f8ecbb` | 2026-11-09 20:53:53 |
| `main/java/com/jianti/app/ui/FamilyActivity.kt` | 6,928 | `23a5b97e6924ee1e699b5d426b8a10bb29f53ee84b76d7ec8b18eb0df637c785` | 2026-11-09 20:53:53 |
| `main/java/com/jianti/app/ui/LoginActivity.kt` | 3,817 | `fabca01e5d011bb3ce14fdfcda9ca9bee8afd034cbe1f2bd4f5e794fc6139949` | 2026-11-09 20:53:44 |
| `main/java/com/jianti/app/ui/ProfileActivity.kt` | 2,581 | `aee6fad54b04d35b0a0c337d39517c7771b94d223a9792f2763984fd7049f116` | 2026-11-09 20:53:52 |
| `main/java/com/jianti/app/ui/SolveActivity.kt` | 4,340 | `a7ed4cf4883d38196f86a24557347a90433311e8579d0f897997b661cab16035` | 2026-11-09 22:46:05 |
| `main/java/com/jianti/app/ui/UiExt.kt` | 1,566 | `a1847e87ac89cf84a0b22fca2dff4e3219fb507054675d253fef4d55c48c84f5` | 2026-11-09 19:37:16 |
| `main/res/drawable/bg_accent_button.xml` | 227 | `c711c57a10d998a6dbbc64a27fbd7f830d8ffa7441de9c2b0c2ba6cb582c63c7` | 2026-11-09 02:30:38 |
| `main/res/drawable/bg_card.xml` | 225 | `ef073f00b5020f5971e45affa5bad82c14104e39f02d4e185674707084e398bc` | 2026-11-09 02:30:38 |
| `main/res/drawable/bg_divider.xml` | 190 | `4d4d44e32e20496eca629aa5c7211c0235a13a47a3fbb16e9303a8321e28ce2d` | 2026-11-09 02:30:40 |
| `main/res/drawable/bg_edit_text.xml` | 292 | `1f4e50ab376609758b0489e62cd28a85024fbf33d26dc4d4d032e8bc3b3e6f57` | 2026-11-09 02:30:38 |
| `main/res/layout/activity_bind.xml` | 4,190 | `6c2940a1cbfbd803b3e49e27c811d5aeb963a37720082ad3e6edc8e7afa6d97b` | 2026-11-09 08:58:40 |
| `main/res/layout/activity_family.xml` | 5,561 | `6c6e13b8434a45b52db6ee87ec0bde239391d88ec70760e8c6ac38979cf02e70` | 2026-11-09 08:59:13 |
| `main/res/layout/activity_login.xml` | 4,274 | `c6a0475af03a8f62f1367def561206fee863f5a963243a03c2f50d20bde6d586` | 2026-11-09 22:57:31 |
| `main/res/layout/activity_profile.xml` | 5,426 | `8ed235563e4f390614d9178d7310e9028fd2d59048a4050301ee3478ef693eb5` | 2026-11-09 17:58:39 |
| `main/res/layout/activity_solve.xml` | 5,945 | `01ce47ecefe19c96cb64ee9659fd8b7088f85254c97f6b8b490c16287abc9ee1` | 2026-11-09 22:46:29 |
| `main/res/layout/top_bar.xml` | 884 | `54f204a439e80cdcf5891dd55c8169f00a1baa20a5e65961a1a97257bc2c883d` | 2026-11-09 08:57:45 |
| `main/res/layout-land/activity_login.xml` | 4,774 | `d4b8dc7a3d82c75b981b2d17c20cffd1a5976966366ebcb322ef16f19aa8d3dc` | 2026-11-09 21:13:54 |
| `main/res/values/colors.xml` | 792 | `fdfbd5dd67cd01e05cdf387360514c1f1403be795b3af50233f8800aaa1820c9` | 2026-11-09 02:30:26 |
| `main/res/values/strings.xml` | 2,881 | `38dc2b99a9043882b1dd164f1124f546c55cbfb504c317625cd51ec3481751ef` | 2026-11-09 22:46:18 |
| `main/res/values/themes.xml` | 2,382 | `355915220a949019b918902eb8e668a04621b2ed6c888fdc9662d1b0c9354328` | 2026-11-09 19:37:46 |

### HarmonyOS ArkTS 壳

- 根目录：`F:/AI/jianti/app/shell`；文件数 51；已计字节 4,636,528；版本标识：**2.0 线**
- 下表路径为相对该根目录的路径（正斜杠）。

| 相对路径 | 字节 | SHA-256 | 修改时间 |
| --- | ---: | --- | --- |
| `build-profile.json5` | 1,632 | `9bbd451517c9aa8f4964153f4ed2d26a8be73159ade8036b07fb9eb1e10d37d8` | 2026-10-22 22:31:56 |
| `hvigorfile.ts` | 113 | `86ea315e8dc733588159b80f6170fc796225672e369796c99472f543a6b0e4dc` | 2026-10-22 21:32:34 |
| `hvigorw` | 1,672 | `6447d9acfff04aa14691baeb11df7d918f55df57c98be9967d62ff4541f83aa0` | 2026-09-22 03:23:33 |
| `hvigorw.bat` | 1,650 | `74e3c1aaa9b91fa11f8a9c6eea52c5eb1de53783991055e05e507ff15fd6731e` | 2026-09-22 03:23:33 |
| `hvigorw.js` | 1,180,396 | `468c9cdbb1dc43799a2024fea63c6c5f60993e3837b4a4d131b14680aac1aa05` | 2026-09-22 03:23:33 |
| `local.properties` | 29 | `5887208dee97aeb80fe3895591eee0f49c40e1645c827e027fcecfb284d6560c` | 2026-10-22 22:11:36 |
| `oh-package.json5` | 236 | `d9790247126022bedc0f53b5f66b4d8b3fbe4fdd4845c71f76157cd9d92efc56` | 2026-10-22 22:14:12 |
| `.hvigor/cache/file-cache.json` | 100,623 | `f50156efdd2b41cfd291eee82da83ff50b5221f88cc910f7e8ce07b4876dffa5` | 2026-11-04 21:00:15 |
| `.hvigor/cache/last-build-info.json` | 21 | `18d7a2a7deb9091af68cbf81f9449ed2c03ba4d353d31a3dc94f6e3e9fb34909` | 2026-11-04 21:00:15 |
| `.hvigor/cache/meta.json` | 97 | `c56d22b77f506a0b8a86c0a9c01c1068661c90c78dcd13def9ec77851480d1d8` | 2026-11-04 21:00:15 |
| `.hvigor/cache/project-config.json` | 6,405 | `3394585b8917070caea0873ea323f3f5aefcfc6653f20a4f644a2856c5749c57` | 2026-11-08 06:00:48 |
| `.hvigor/cache/task-cache.json` | 259,057 | `1fdeb2349fe4aac2921bd70d84f0d435c8b57c2b39cd5d708d56cee51042c491` | 2026-11-04 21:00:15 |
| `.hvigor/dependencyMap/dependencyMap.json5` | 236 | `11a5ab8b723b35659dde32acd3243eb81faad1a56e5f23f2e4cb1c4d6b12026b` | 2026-11-08 05:59:24 |
| `.hvigor/dependencyMap/oh-package.json5` | 187 | `a8d9b01d43d03daeddc4c6145f2a64a3a9ea54e93a1b21c7d83dc6de743cdc56` | 2026-10-22 22:15:21 |
| `.hvigor/dependencyMap/entry/oh-package.json5` | 127 | `3b237bb38810ed285c09a541edbbd2f1250e6dc932c7f59e2141bbcae1e3e7ff` | 2026-10-22 22:15:21 |
| `.hvigor/outputs/build-logs/build.log` | 1,537,143 | `24af3824f5dcc90e69815646fc6d80c73b1823ebecf24d605820179d3be193b9` | 2026-11-04 21:00:15 |
| `.hvigor/outputs/logs/details/details.json` | 2,783 | `1093aed1b1d64eca3c1b1ca8c8cca573a53e7914c7cfd4b4950ccc5138c70e26` | 2026-11-04 21:00:15 |
| `.hvigor/outputs/records/performance-recorder.json` | 964 | `7294077d825cbebb02daeda08b1b51f22387abc2f41c9501220c0206161a4d41` | 2026-11-08 06:00:58 |
| `.hvigor/report/report-202611042100109330.json` | 315,402 | `2d4c5679f1d948b1bb1284211457d89c32a49604c0af2db61ab43ee8d002fae5` | 2026-11-04 21:00:15 |
| `.hvigor/report/report-202611080556279230.json` | 30,649 | `0451dfe5f16f2c14d94d188c4149695ba4d0ef767aabbf9c8aecaafbb09dbe4a` | 2026-11-08 05:56:29 |
| `.hvigor/report/report-202611080556424020.json` | 30,674 | `3fa19fb7525e88d05b0e849f17df7b8666d13ab1729a9dba331c3bb632e42948` | 2026-11-08 05:56:43 |
| `.hvigor/report/report-202611080559336480.json` | 33,795 | `7f99aee4ed0ce31c0809467cf5f3695be9ecf42a5c0dda7158814e6b5ecb46db` | 2026-11-08 05:59:34 |
| `.hvigor/report/report-202611080559445410.json` | 95,531 | `457b4e68982bb62d5f90ec2a7237f768ac27133a2d7759cd98b0d22a0f1653bf` | 2026-11-08 05:59:45 |
| `.hvigor/report/report-202611080559522790.json` | 33,955 | `e9c0bd3dbce22bff7eb055020e6c50a53cc7acf1acc8f6c648ebf5db2b759936` | 2026-11-08 05:59:53 |
| `.hvigor/report/report-202611080600127450.json` | 33,989 | `1ba82f12f1ad8fcbca9095ec1cac351b76b51b21571d9348ea0c89e306f0e9ab` | 2026-11-08 06:00:13 |
| `.hvigor/report/report-202611080600461920.json` | 348,005 | `f18960f4fcf6be26d05baef12375e1d17b2e97fca82c93132d3caa10fbb90b09` | 2026-11-08 06:00:58 |
| `.hvigor/report/report-202611080601299940.json` | 270,672 | `1333c0c9ed89714c879982732c8fe4999ae012b7917d683b9939efc30a7d977d` | 2026-11-08 06:01:35 |
| `.hvigor/report/report-202611080617492630.json` | 251,680 | `a4ffa0b8349a8c0c2538d54c57e677c6eb0061075a0498ebb35ec6d00edb26d9` | 2026-11-08 06:17:51 |
| `.idea/.gitignore` | 50 | `a733dbdc2ce56ea1138c7261582b33d325b8aca0ed8fc04c4dbf172e3492912a` | 2026-10-22 22:19:41 |
| `.idea/modules.xml` | 419 | `2a67f7f63d32674716aeeb2a4fbd3e761e7a167309cfa6b6dce0941d4f8b24e2` | 2026-10-22 22:19:41 |
| `.idea/workspace.xml` | 6,140 | `99fc3a7e4c716ac2aac660c1f6c18495fa1f84de7fb5e80775de2cbef9891b35` | 2026-10-23 03:44:51 |
| `.idea/.deveco/project.cache.json` | 416 | `f9054fbde160250a99bb4527eb18954562d4a692d6325362c5afaba4019ae5fe` | 2026-10-22 22:20:21 |
| `.idea/.deveco/cxx/.clang-format` | 3,500 | `8850f816112de84457ac4f2b751edfdcd938b02511875b93e8d5d2eda90b8bed` | 2026-10-23 01:04:02 |
| `.idea/.deveco/module/entry.cache.json` | 1,753 | `25dfa916dcabaa116a69c22c263e2f891ed0807491a786303df21cb4bb6a0949` | 2026-10-22 22:20:21 |
| `.idea/modules/JianTiShell.iml` | 2,433 | `1ec7d19a49ec914f9d51862a72b835fe0eee4db74506200f3c1ad4222e2f03de` | 2026-10-22 22:19:41 |
| `.idea/modules/entry.iml` | 262 | `23ea5da2060c2252298dbcb9c6c469d780c2e7d70ed1eab266bdc13e167867ea` | 2026-10-22 22:19:41 |
| `AppScope/app.json5` | 210 | `ea65c802b0c88187e86011e311bf0bf664598ecd538f4717386960250bece655` | 2026-10-22 21:32:46 |
| `AppScope/resources/base/element/string.json` | 92 | `63e43c065f846b5bff57f3ede6efacfa3964be557b005a5e3e76e77d94cd856d` | 2026-10-24 14:59:07 |
| `AppScope/resources/base/media/app_icon.png` | 2,148 | `9438fb920ceaabb7c7b4e301631e742a8b5da503f6f3d4073f4ed9ab072ad3e1` | 2026-10-22 21:35:18 |
| `entry/build-profile.json5` | 323 | `c2adcd29fb4cf2536fcb1c009b175f6eeca93bdd0b7544bff1d05eccb3693f94` | 2026-10-22 22:10:57 |
| `entry/hvigorfile.ts` | 113 | `3519dd0c8deb5616cde0c847dc04c09b59d9febda403d750795b34949ce5645b` | 2026-10-22 21:33:17 |
| `entry/oh-package.json5` | 166 | `93bb82e43e3f4a7a8b2a23bab89a11200873fd5a4b7e7c982426ddfdad9bcf15` | 2026-10-22 21:33:24 |
| `entry/src/main/module.json5` | 1,066 | `a79abd4a2ca3bb9da101963e1cd637fed715fe3c3cabbb096fee270f657ce1f0` | 2026-10-24 18:19:48 |
| `entry/src/main/ets/entryability/EntryAbility.ets` | 1,016 | `6cdc3305e3052db2232ad93fb1ca64d534e7bfe4801fee1795aba90b2ef737a0` | 2026-10-25 01:45:29 |
| `entry/src/main/ets/pages/Index.ets` | 75,976 | `d4ccc2fed6dde74c86da248bb8fdc6817dc7793309eee5e45febf8cc5e31b746` | 2026-10-29 02:28:01 |
| `entry/src/main/resources/base/element/color.json` | 107 | `adb9fd6200ccb1a9e9c4be2c98a9e39a0e8929ea047701f11dc4115dcd181a46` | 2026-10-22 21:33:52 |
| `entry/src/main/resources/base/element/string.json` | 269 | `860490cbf2cc601c026e1d3028339320586b146bfccb76449ccfae5a4b117a8b` | 2026-10-24 14:59:06 |
| `entry/src/main/resources/base/media/startIcon.png` | 2,148 | `9438fb920ceaabb7c7b4e301631e742a8b5da503f6f3d4073f4ed9ab072ad3e1` | 2026-10-22 21:35:18 |
| `entry/src/main/resources/base/profile/main_pages.json` | 42 | `b7248e3d58d4006a4bb01d9991e34b1292c03f3d212b5945470ec5bdcf9f1cbd` | 2026-10-22 21:33:54 |
| `hvigor/hvigor-config.json5` | 156 | `2a16ced44f98ea805010f61da78d03b40c7894ae21f6100b5eff83b5d97ef0be` | 2026-10-22 22:10:36 |
| `hvigor/hvigor-wrapper.js` | 0 | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` | 2026-09-22 03:23:33 |

### 产品文档目录

- 根目录：`F:/AIsoft/data/简题2.0/codex`；文件数 7；已计字节 35,198；版本标识：**v1.1 / v1.0**
- 下表路径为相对该根目录的路径（正斜杠）。

| 相对路径 | 字节 | SHA-256 | 修改时间 |
| --- | ---: | --- | --- |
| `简题2.0_升级功能清单_20261109.md` | 3,228 | `f18a8819986fc0bbf439a4906ae24447e951b231e6f158952b2f124f1dda5b70` | 2026-11-09 17:43:33 |
| `简题2.0_开发任务_第30批_Codex_App重新设计5稿.md` | 2,647 | `608bbb3fc37b8b187f3513942bb04d72f068ffd0105bd2b72e58fe95c1c51eac` | 2026-11-09 14:15:57 |
| `简题2.0_开发任务_第39批_Codex_产品优势分析.md` | 1,918 | `504d89ba48ccde08dc2bde7302db1c81aba19d2fdbe48f7dd334480bf8049890` | 2026-11-09 17:43:39 |
| `简题2.0_开发任务_第43批_Codex_质疑Nyx产品文档.md` | 1,421 | `ab6ed1f4abe33bc685990f4e867c2ba03a822c689808e8a1e31b58cee72e1193` | 2026-11-09 20:54:09 |
| `简题2.0_开发任务_第45批_Codex_Artemis试跑.md` | 2,090 | `92eac3ad9053b1d3d5d470aa89d2a93b5e3c4c2d29baf2a9e94680e82ec54fae` | 2026-11-09 21:49:53 |
| `简题App2.0_UIUE交互规范_20261109.md` | 12,060 | `bea7b3879b73efde03337f05e1dd7617dc66d5af049b286a26f8ef4a1052f88f` | 2026-11-09 13:06:09 |
| `简题App2.0_需求分析_20261109.md` | 11,834 | `dfe62d5c8ec4fc6e1299ada4c4e9ce4cf69673b3bba8458c142068e0cf7bf02d` | 2026-11-09 13:06:19 |

## 三、本批「推了什么／没推什么」

| 类别 | 处理方式 | 理由 |
| --- | --- | --- |
| 版本管理元信息（本清单、办法、说明、核查表、证据 JSON、manifest） | **推** | 纯文本、体量小、不含密钥，属「版本备份」本职 |
| 网关 v3.23 / Android 源码 / 鸿蒙壳 / APK / 产品文档原文 | **本批不推** | ① 任务边界「只做版本管理，不改代码、不改产品文档内容」；② 30 GB 级素材含 `.venv`／build／截图／题库／视频，未定哪些该进仓；③ 与 Gitee 主账本的权威关系未裁定。清单已留逐文件 SHA，将来可随时按裁定结果补推并复算 |
| Gitee 主账本 | **不读不推** | 边界：Gitee 为主账本，本批核的是 GitHub↔本地差距 |
| 上游既有文件（`server/gateway.py` 等） | **不改** | 本批不 rewrite 历史、不 force push |

> 「能推的先推」按补充令执行：把**能安全推的元信息**推上去；推不动的写清卡点，不虚报。详见 `05_PUSH_RECEIPT.md`。

## 四、复算方法

```python
import hashlib, json
d = json.load(open('12_EVIDENCE_LOCAL_BASELINE.json', encoding='utf-8'))
for g in d['groups'].values():
    for f in g['files']:
        h = hashlib.sha256(open(f['path'], 'rb').read()).hexdigest()
        assert h == f['sha256']  # 与清单逐件比对
```

- 上游侧对照：`13_EVIDENCE_UPSTREAM_SHA.json` 的 5 件 SHA 可用 `raw.githubusercontent.com/hhrimmon/jianti-subtraction/main/<path>` 重下复算。
- 差距判定见 `01_STATUS_CHECK.md` 第三节 D1–D8。