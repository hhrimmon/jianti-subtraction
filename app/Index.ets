import { camera, cameraPicker } from '@kit.CameraKit';
import { image } from '@kit.ImageKit';
import { http } from '@kit.NetworkKit';
import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { util } from '@kit.ArkTS';
import { promptAction } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';

const TAG: string = 'JianTi';

function jlog(msg: string): void {
  hilog.info(0x0001, TAG, msg);
}

const DEFAULT_URL: string = 'http://127.0.0.1:8000/solve';
const MAX_BYTES: number = 2 * 1024 * 1024;

interface ShotResp {
  label?: string;
  png_b64?: string;
}

interface SolveResponse {
  ok?: boolean;
  text?: string;
  error?: string;
  upload_ms?: number;
  answer_ms?: number;
  shots?: ShotResp[];
}

class ShotView {
  label: string = '';
  pm: image.PixelMap | null = null;
  aspect: number = 1.4;
}

/// 一题一显的展示单元：题号+括号小题为 qid；过程与答案锁定，用卡后才进导航
class Card {
  label: string = '';
  qid: string = '';
  kind: string = '思路'; // 思路 | 过程与答案
  pm: image.PixelMap | null = null;
  aspect: number = 1.4;
  text: string = '';
  locked: boolean = false;
}

/// 留痕记录（只追加不回改）：advance=预支1张 / request=向家长申请 / approved=家长批准到账
interface RecItem {
  type: string;
  ts: number;
  amount: number;
  reqId: string;
}

/// /card/apply 返回
interface CardApplyResp {
  ok?: boolean;
  application_id?: string;
  status?: string;
  dup?: boolean;
}

/// /card/preborrow、/card/unlock 返回
interface CardPreborrowResp {
  ok?: boolean;
  remaining?: number;
  preborrow_total?: number;
  device_id?: string;
  unlocked?: boolean;
  charged?: boolean;
  dup?: boolean;
  reason?: string;
}

/// jt_state.json 磁盘状态
interface StateFile {
  cardsLeft?: number;
  records?: string;
  consumed?: string;
  pendingReqId?: string;
  serverUrl?: string;
  pwd?: string;
  maxPreborrow?: number;
  preborrowDay?: string;
  preborrowToday?: number;
}

/// 卡账本：单条用卡记录（/card/status 的 records 行）
interface CardRec {
  ts?: number;
  time?: string;
  device_id?: string;
  qkey?: string;
  action?: string; // unlock | preborrow | apply | approve | cancel | dev_reset
  delta?: number;
  balance?: number;
  note?: string;
}

/// 卡账本：待批申请单
interface CardApp {
  application_id?: string;
  qkey?: string;
  time?: string;
  status?: string;
}

/// GET /card/status 返回
interface CardStatusResp {
  ok?: boolean;
  gateway?: string;
  device_id?: string;
  day?: string;
  balance?: number;
  daily_quota?: number;
  used_today?: number;
  preborrow_total?: number;
  pending_count?: number;
  pending_applications?: CardApp[];
  records?: CardRec[];
}

@Entry
@Component
struct Index {
  @State serverUrl: string = DEFAULT_URL;
  @State resultText: string = '';
  @State statusText: string = '待拍题';
  @State resultTitle: string = '完成';
  @State timingText: string = '';
  @State shots: ShotView[] = [];
  @State navCards: Card[] = []; // 当前导航可见的卡：思路 + 已解锁的过程与答案
  @State cardIdx: number = 0;
  // 卡数持久化：手动读写 filesDir/jt_state.json（PersistentStorage 在本机重启后回默认值，已废弃）
  @State cardsLeft: number = 5;
  @State cardRecordsJson: string = '[]'; // 留痕记录 JSON 数组，只追加
  @State consumedJson: string = '[]'; // 已到账的申请单 id
  @State pendingReqId: string = ''; // 待家长批准的申请单 id（同一时间只允许一张）
  @State showUnlock: boolean = false;
  @State showNoCard: boolean = false; // 卡用完临时弹窗：关弹窗/退出/重进都回思路页
  @State showAdvanceConfirm: boolean = false; // 预支确认弹窗（含提醒）
  @State reqSubmitting: boolean = false;
  @State busy: boolean = false;
  @State isLandscape: boolean = false;
  @State view: string = 'home'; // home | result | admin
  @State adminPage: string = 'menu'; // menu | settings | records（管理终端内部子页）
  // 用卡记录页（读 server 卡账本）：单家庭账号测试版，设备ID只做显示
  @State cardDevId: string = '';
  @State cardBalance: number = 0;
  @State cardQuota: number = 5;
  @State cardUsed: number = 0;
  @State cardPreborrow: number = 0;
  @State cardPendingJson: string = '[]';
  @State cardSrvRecordsJson: string = '[]';
  @State cardLoading: boolean = false;
  @State cardErr: string = '';
  @State cardQuotaEdit: string = '';
  // 预支上限（客户端实现，平台强制校验待后续同步）：默认 5 张＝每日张数 1 倍
  @State cardMaxEdit: string = '';
  @State maxPreborrow: number = 5;
  @State preborrowToday: number = 0;
  @State preborrowDay: string = '';
  // 用卡记录密码门：改每日张数 / 改预支上限须先过密码（不用手机解锁、指纹）
  @State showPwdGate: boolean = false;
  @State pwdInput: string = '';
  @State pwdErr: string = '';
  @State pwdFirstTime: boolean = false;
  private pwd: string = '';
  private pwdAction: string = '';
  private allCards: Card[] = [];
  private emptyCard: Card = new Card();
  private lastKeyTs: number = 0;
  private activeReq: http.HttpRequest | null = null;
  private cancelled: boolean = false;

  /// 状态文件路径（启动时同步读，变更后同步写）
  private statePath(): string {
    return getContext(this).filesDir + '/jt_state.json';
  }

  aboutToAppear(): void {
    this.loadState();
  }

  private loadState(): void {
    try {
      const txt: string = fs.readTextSync(this.statePath());
      const obj = JSON.parse(txt) as StateFile;
      if (typeof obj.cardsLeft === 'number') {
        this.cardsLeft = obj.cardsLeft;
      }
      if (typeof obj.records === 'string') {
        this.cardRecordsJson = obj.records;
      }
      if (typeof obj.consumed === 'string') {
        this.consumedJson = obj.consumed;
      }
      if (typeof obj.pendingReqId === 'string') {
        this.pendingReqId = obj.pendingReqId;
      }
      if (typeof obj.serverUrl === 'string' && obj.serverUrl.trim().length > 0) {
        this.serverUrl = obj.serverUrl;
      }
      if (typeof obj.pwd === 'string') {
        this.pwd = obj.pwd;
      }
      if (typeof obj.maxPreborrow === 'number' && obj.maxPreborrow >= 0) {
        this.maxPreborrow = obj.maxPreborrow;
      }
      if (typeof obj.preborrowDay === 'string') {
        this.preborrowDay = obj.preborrowDay;
      }
      if (typeof obj.preborrowToday === 'number' && obj.preborrowToday >= 0) {
        this.preborrowToday = obj.preborrowToday;
      }
      this.resetPreborrowIfNewDay();
      jlog(`loadState cardsLeft=${this.cardsLeft} pending=${this.pendingReqId} maxPre=${this.maxPreborrow}`);
    } catch (e) {
      jlog(`loadState 无历史或损坏，用默认值: ${e}`);
    }
  }

  private saveState(): void {
    try {
      const body: string = JSON.stringify({
        cardsLeft: this.cardsLeft,
        records: this.cardRecordsJson,
        consumed: this.consumedJson,
        pendingReqId: this.pendingReqId,
        serverUrl: this.serverUrl,
        pwd: this.pwd,
        maxPreborrow: this.maxPreborrow,
        preborrowDay: this.preborrowDay,
        preborrowToday: this.preborrowToday
      });
      const f: fs.File = fs.openSync(this.statePath(),
        fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      fs.writeSync(f.fd, body);
      fs.closeSync(f);
    } catch (e) {
      jlog(`saveState 失败: ${e}`);
    }
  }

  /// 本地日期串 YYYY-MM-DD：预支次数按自然日累计，跨日归零
  private todayStr(): string {
    const d: Date = new Date();
    const p = (n: number): string => (n < 10 ? `0${n}` : `${n}`);
    return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}`;
  }

  /// 次日：预支计数归零，昨日预支不抵扣今日额度（每日张数由网关按日重置）
  private resetPreborrowIfNewDay(): void {
    const today: string = this.todayStr();
    if (this.preborrowDay !== today) {
      this.preborrowDay = today;
      this.preborrowToday = 0;
    }
  }

  /// 当日还剩几张可预支（客户端实现，平台强制校验待后续同步）
  private preborrowRemain(): number {
    const r: number = this.maxPreborrow - this.preborrowToday;
    return r > 0 ? r : 0;
  }

  /// 拍照 -> 压缩到 2MB 以内 -> POST 到网关 -> 显示返回内容
  private async captureAndSend(): Promise<void> {
    if (this.busy) {
      return;
    }
    this.busy = true;
    this.cancelled = false;
    this.resultText = '';
    this.timingText = '';
    this.shots = [];
    this.allCards = [];
    this.navCards = [];
    this.cardIdx = 0;
    this.showUnlock = false;
    this.statusText = '打开相机…';

    try {
      const profile: cameraPicker.PickerProfile = {
        cameraPosition: camera.CameraPosition.CAMERA_POSITION_BACK
      };
      const ctx = getContext(this) as common.UIAbilityContext;
      const picked: cameraPicker.PickerResult = await cameraPicker.pick(
        ctx,
        [cameraPicker.PickerMediaType.PHOTO],
        profile
      );
      if (picked.resultCode !== 0 || !picked.resultUri) {
        this.statusText = '已取消拍照';
        this.view = 'home';
        promptAction.showToast({ message: '已取消拍照' });
        return;
      }

      this.statusText = '压缩图片…';
      const jpeg: ArrayBuffer = await this.compressToJpeg(picked.resultUri);
      const kb: number = Math.round(jpeg.byteLength / 1024);
      this.statusText = `上传中…（${kb} KB）`;

      const url: string = this.serverUrl.trim();
      const req = http.createHttp();
      this.activeReq = req;
      try {
        jlog(`发送请求 ${url} body=${kb}KB`);
        const resp: http.HttpResponse = await req.request(url, {
          method: http.RequestMethod.POST,
          header: { 'Content-Type': 'image/jpeg' },
          extraData: jpeg,
          connectTimeout: 15000,
          readTimeout: 330000,
          usingCache: false,
          expectDataType: http.HttpDataType.STRING
        });
        jlog(`收到响应 code=${resp.responseCode}`);

        if (resp.responseCode !== 200) {
          this.statusText = `网关返回 HTTP ${resp.responseCode}`;
          this.resultTitle = '失败';
          this.resultText = String(resp.result);
          this.view = 'result';
          await this.refreshCardStatus();
          return;
        }

        const body: string = String(resp.result);
        jlog(`响应体 ${body.length} 字符，开始解析`);
        try {
          const parsed = JSON.parse(body) as SolveResponse;
          this.resultText = parsed.text ?? body;
          jlog(`解析完成 shots=${parsed.shots ? parsed.shots.length : 0}`);
          if (parsed.shots && parsed.shots.length > 0) {
            this.shots = await this.decodeShots(parsed.shots);
            jlog(`解码完成 ${this.shots.length} 张`);
          }
          this.buildCards();
          jlog(`建卡完成 nav=${this.navCards.length}，切结果页`);
          if (typeof parsed.upload_ms === 'number' && typeof parsed.answer_ms === 'number') {
            const up: number = Math.round(parsed.upload_ms / 1000);
            const ans: number = Math.round(parsed.answer_ms / 1000);
            this.timingText = `${up}+${ans}S`;
          }
        } catch (e) {
          jlog(`解析/解码异常: ${e}`);
          this.resultText = body;
        }
        this.statusText = `完成（${kb} KB）`;
        this.resultTitle = '完成';
        this.view = 'result';
        // 每次进结果页都从网关刷新一次卡账本（家长可能已批准），不只用本地缓存
        await this.refreshCardStatus();
      } finally {
        this.activeReq = null;
        req.destroy();
      }
    } catch (err) {
      if (this.cancelled) {
        // 用户主动取消：回拍摄页，不进失败页
        this.statusText = '已取消，可重拍';
        this.view = 'home';
        return;
      }
      const stage: string = this.statusText;
      const e = err as BusinessError;
      const code: string = e && e.code !== undefined ? `${e.code}` : 'n/a';
      const msg: string = e && e.message ? e.message : String(err);
      this.statusText = '失败';
      this.resultTitle = '失败';
      this.resultText = `错误[${code}]：${msg}\n阶段：${stage}`;
      this.view = 'result';
    } finally {
      this.busy = false;
      this.activeReq = null;
    }
  }

  /// 识别中点「取消」：掐断请求、状态复位回拍摄页，不许无限卡识别中
  private cancelRequest(): void {
    this.cancelled = true;
    const req: http.HttpRequest | null = this.activeReq;
    this.activeReq = null;
    if (req !== null) {
      try {
        req.destroy();
      } catch (e) {
      }
    }
    this.busy = false;
    this.statusText = '已取消，可重拍';
    this.view = 'home';
  }

  /// 解码 -> 缩边 -> JPEG 打包，循环降质量直到 <= 2MB
  private async compressToJpeg(uri: string): Promise<ArrayBuffer> {
    const src: image.ImageSource = this.openImageSource(uri);
    const info: image.ImageInfo = await src.getImageInfo();

    let side: number = Math.max(info.size.width, info.size.height);
    let scale: number = side > 1600 ? 1600 / side : 1;
    let pixelMap: image.PixelMap = await this.decodeScaled(src, info, scale);

    const packer: image.ImagePacker = image.createImagePacker();
    let quality: number = 85;
    let out: ArrayBuffer = await packer.packing(pixelMap, { format: 'image/jpeg', quality: quality });

    while (out.byteLength > MAX_BYTES) {
      if (quality > 30) {
        quality -= 15;
        out = await packer.packing(pixelMap, { format: 'image/jpeg', quality: quality });
      } else {
        scale = scale * 0.7;
        pixelMap = await this.decodeScaled(src, info, scale);
        quality = 70;
        out = await packer.packing(pixelMap, { format: 'image/jpeg', quality: quality });
      }
      if (scale < 0.2) {
        break;
      }
    }

    await packer.release();
    await src.release();
    return out;
  }

  /// cameraPicker 返回的是媒体库 URI（file://media/...），createImageSource(uri) 只认应用沙箱路径、
  /// 失败时返回 undefined 而不抛错，所以必须先按文件描述符打开。
  private openImageSource(uri: string): image.ImageSource {
    const f: fs.File = fs.openSync(uri, fs.OpenMode.READ_ONLY);
    const byFd: image.ImageSource | undefined = image.createImageSource(f.fd);
    if (byFd !== undefined && byFd !== null) {
      return byFd;
    }
    const byUri: image.ImageSource | undefined = image.createImageSource(uri);
    if (byUri !== undefined && byUri !== null) {
      return byUri;
    }
    throw new Error(`createImageSource 返回空（fd=${f.fd}）：${uri}`);
  }

  private async decodeScaled(src: image.ImageSource, info: image.ImageInfo, scale: number): Promise<image.PixelMap> {
    const opts: image.DecodingOptions = {
      desiredSize: {
        width: Math.max(1, Math.round(info.size.width * scale)),
        height: Math.max(1, Math.round(info.size.height * scale))
      }
    };
    return await src.createPixelMap(opts);
  }

  /// base64 PNG -> PixelMap，逐段解码；单段失败不影响其它段
  private async decodeShots(list: ShotResp[]): Promise<ShotView[]> {
    const out: ShotView[] = [];
    for (const s of list) {
      const v = new ShotView();
      v.label = s.label ?? '';
      try {
        const b64: string = s.png_b64 ?? '';
        if (b64.length === 0) {
          continue;
        }
        const pm: image.PixelMap | null = await this.decodeWithTimeout(b64, 8000);
        if (pm === null) {
          jlog(`解码超时跳过: ${v.label}`);
          continue;
        }
        v.pm = pm;
        const inf: image.ImageInfo = await pm.getImageInfo();
        v.aspect = inf.size.width / Math.max(1, inf.size.height);
        out.push(v);
      } catch (e) {
        jlog(`解码失败跳过: ${v.label}`);
      }
    }
    return out;
  }

  /// 单张图解码带超时：解码挂起时 8 秒后放弃，不许拖死整个结果页
  private decodeWithTimeout(b64: string, ms: number): Promise<image.PixelMap | null> {
    return new Promise<image.PixelMap | null>((resolve) => {
      const timer: number = setTimeout(() => {
        resolve(null);
      }, ms);
      try {
        const bytes: Uint8Array = new util.Base64Helper().decodeSync(b64);
        const src: image.ImageSource = image.createImageSource(bytes.buffer as ArrayBuffer);
        src.createPixelMap().then((pm: image.PixelMap) => {
          clearTimeout(timer);
          src.release();
          resolve(pm);
        }).catch(() => {
          clearTimeout(timer);
          resolve(null);
        });
      } catch (e) {
        clearTimeout(timer);
        resolve(null);
      }
    });
  }

  /// 有图用图，没图把文本按小题切卡；过程与答案一律锁定，用卡解锁后才进导航
  private buildCards(): void {
    const cards: Card[] = [];
    if (this.shots.length > 0) {
      for (const s of this.shots) {
        const c = new Card();
        c.label = s.label;
        const dot: number = s.label.indexOf('·');
        c.qid = dot > 0 ? s.label.substring(0, dot) : s.label;
        c.kind = s.label.indexOf('思路') >= 0 ? '思路' : '过程与答案';
        c.pm = s.pm;
        c.aspect = s.aspect;
        cards.push(c);
      }
    } else {
      const parsed: Card[] = this.cardsFromText(this.resultText);
      for (const c of parsed) {
        cards.push(c);
      }
    }
    if (cards.length === 0) {
      const c = new Card();
      c.label = '答案';
      c.qid = '整卷';
      c.kind = '思路';
      c.text = '（网关未返回内容）';
      cards.push(c);
    }
    // 整卷总思路不成卡、不计题数：无题号卡（整卷/题目头）直接丢弃；全丢光时兜底保留原列表
    // 有大题的小题块（第N题(k)）时，丢弃大题级重复块：模型常在小题答完后又按题级总结一遍
    const subMains: string[] = [];
    for (const c of cards) {
      const sm: RegExpMatchArray | null = c.qid.match(/^第(.+)题\(\d+\)$/);
      if (sm && subMains.indexOf(sm[1]) < 0) {
        subMains.push(sm[1]);
      }
    }
    const real: Card[] = [];
    for (const c of cards) {
      c.locked = (c.kind === '过程与答案');
      if (c.qid.length === 0 || c.qid === '整卷') {
        continue;
      }
      const bm: RegExpMatchArray | null = c.qid.match(/^第(.+)题$/);
      if (bm && subMains.indexOf(bm[1]) >= 0) {
        continue;
      }
      real.push(c);
    }
    this.allCards = real.length > 0 ? real : cards;
    this.cardIdx = 0;
    this.refreshNav();
  }

  /// 文本降级切卡：认 【标题】/### 标题/第N题/题目N/(k) 开头的行作为新卡；裸 (k) 继承最近大题号→第N题(k)
  private cardsFromText(text: string): Card[] {
    const out: Card[] = [];
    if (!text || text.trim().length === 0) {
      return out;
    }
    const headRe: RegExp = new RegExp('^\\s*(#{1,6}\\s*\\S|【[^】]{1,30}】|第\\s*[0-9一二三四五六七八九十]+\\s*题|题目\\s*[0-9一二三四五六七八九十]+|[（(]\\d{1,2}[）)]\\s*\\S)');
    const qidRe: RegExp = new RegExp('(?:第\\s*([0-9一二三四五六七八九十]+)\\s*题|题目\\s*([0-9一二三四五六七八九十]+))\\s*(?:[（(]\\s*(\\d{1,2})\\s*[）)])?');
    const subHeadRe: RegExp = new RegExp('^[（(]\\s*(\\d{1,2})\\s*[）)]');
    let cur: Card | null = null;
    let lastMainQid: string = ''; // 最近大题号（第N题，不带括号），裸 (k) 小题头继承它
    const lines: string[] = text.split('\n');
    for (const line of lines) {
      const t: string = line.trim();
      if (t.length === 0) {
        continue;
      }
      const subM: RegExpMatchArray | null = t.match(subHeadRe);
      const isSubHead: boolean = subM !== null && lastMainQid.length > 0;
      if ((headRe.test(t) && t.length <= 60) || isSubHead) {
        cur = new Card();
        cur.label = t.replace(new RegExp('[#【】]', 'g'), '').trim();
        const m: RegExpMatchArray | null = t.match(qidRe);
        if (m) {
          const qn: string = m[1] !== undefined ? m[1] : (m[2] !== undefined ? m[2] : '');
          cur.qid = qn.length > 0 ? `第${qn}题` : '';
          if (cur.qid.length > 0) {
            lastMainQid = cur.qid;
          }
          if (m[3] !== undefined && cur.qid.length > 0) {
            cur.qid = `${cur.qid}(${m[3]})`;
          }
        } else if (isSubHead && subM !== null) {
          // 裸 (k) 小题头：不继承就丢/堆，必须挂到最近大题下
          cur.qid = `${lastMainQid}(${subM[1]})`;
        }
        if (t.indexOf('思路') >= 0 || t.indexOf('分析') >= 0) {
          cur.kind = '思路';
        } else if (t.indexOf('过程') >= 0 || t.indexOf('答案') >= 0 || t.indexOf('步骤') >= 0 || t.indexOf('解答') >= 0) {
          cur.kind = '过程与答案';
        } else if (out.length > 0) {
          cur.kind = out[out.length - 1].kind;
        }
        if (isSubHead && subM !== null) {
          // 裸 (k) 行同时是正文首行：去掉 (k) 前缀，余下文字进正文；标签规范为「第N题(k) 栏目」
          const rest: string = t.replace(subHeadRe, '').trim();
          cur.label = `${cur.qid} ${cur.kind}`;
          if (rest.length > 0) {
            cur.text = rest;
          }
        }
        out.push(cur);
        continue;
      }
      if (cur === null) {
        cur = new Card();
        cur.label = '题目';
        out.push(cur);
      }
      cur.text = cur.text.length > 0 ? `${cur.text}\n${t}` : t;
    }
    return out;
  }

  /// 小题序列：按 allCards 里 qid 首次出现的顺序（题号口径，与思路/过程模式无关）
  private navQids(): string[] {
    const qids: string[] = [];
    for (const c of this.allCards) {
      if (qids.indexOf(c.qid) < 0) {
        qids.push(c.qid);
      }
    }
    return qids;
  }

  private navQCount(): number {
    return this.navQids().length;
  }

  /// 当前小题在小题序列里的下标；没有卡时回 -1
  private curQIndex(): number {
    if (this.navCards.length === 0) {
      return -1;
    }
    return this.navQids().indexOf(this.curCard().qid);
  }

  /// 跳到指定小题：优先落该题思路卡，没有则落该题第一张卡（不跨题）
  private jumpToQid(qid: string): void {
    let fallback: number = -1;
    for (let i = 0; i < this.navCards.length; i++) {
      if (this.navCards[i].qid !== qid) {
        continue;
      }
      if (this.navCards[i].kind === '思路') {
        this.cardIdx = i;
        return;
      }
      if (fallback < 0) {
        fallback = i;
      }
    }
    if (fallback >= 0) {
      this.cardIdx = fallback;
    }
  }

  /// 导航列表按小题主序重建：每题先思路卡、再已解锁的过程与答案卡。
  /// 重建后尽量保住「当前小题 + 当前模式」，找不到就保小题，再不行夹到末位。
  private refreshNav(): void {
    const keepQid: string = this.curCard().qid;
    const keepKind: string = this.curCard().kind;
    const list: Card[] = [];
    for (const qid of this.navQids()) {
      let thought: number = -1;
      const procs: number[] = [];
      for (let i = 0; i < this.allCards.length; i++) {
        const c: Card = this.allCards[i];
        if (c.qid !== qid) {
          continue;
        }
        if (c.kind === '思路') {
          if (thought < 0) {
            thought = i;
          }
        } else if (!c.locked) {
          procs.push(i);
        }
      }
      if (thought >= 0) {
        list.push(this.allCards[thought]);
      }
      for (const pi of procs) {
        list.push(this.allCards[pi]);
      }
    }
    if (list.length === 0) {
      list.push(this.emptyCard);
    }
    this.navCards = list;
    let hit: number = -1;
    for (let i = 0; i < list.length; i++) {
      if (list[i].qid === keepQid && list[i].kind === keepKind) {
        hit = i;
        break;
      }
    }
    if (hit < 0) {
      for (let i = 0; i < list.length; i++) {
        if (list[i].qid === keepQid) {
          hit = i;
          break;
        }
      }
    }
    if (hit >= 0) {
      this.cardIdx = hit;
    } else if (this.cardIdx >= list.length) {
      this.cardIdx = list.length - 1;
    }
  }

  private curCard(): Card {
    if (this.cardIdx >= 0 && this.cardIdx < this.navCards.length) {
      return this.navCards[this.cardIdx];
    }
    return this.emptyCard;
  }

  /// 上一题/下一题只看小题下标与小题总数，不看当前是思路还是过程
  private goPrev(): void {
    const qi: number = this.curQIndex();
    if (qi <= 0) {
      return;
    }
    this.jumpToQid(this.navQids()[qi - 1]);
  }

  private goNext(): void {
    const qi: number = this.curQIndex();
    if (qi < 0 || qi >= this.navQCount() - 1) {
      return;
    }
    this.jumpToQid(this.navQids()[qi + 1]);
  }

  /// n/N 按小题算：同一小题的思路卡与过程卡共享一个序号，N=小题总数（不按卡数翻倍）
  private navPosText(): string {
    const total: number = this.navQCount();
    if (total === 0) {
      return '0/0';
    }
    const i: number = this.curQIndex();
    return `${i >= 0 ? i + 1 : 0}/${total}`;
  }

  private hasLockedAnswer(qid: string): boolean {
    for (const c of this.allCards) {
      if (c.qid === qid && c.locked) {
        return true;
      }
    }
    return false;
  }

  /// 触屏点「解题」或物理确定键双击都会走到这里。
  /// 点解题先判卡：每次先从网关刷新（家长可能已批准），有卡走确认用卡，0 卡进卡用完弹窗
  private async tryOpenUnlock(): Promise<void> {
    if (this.showUnlock || this.showNoCard || this.showAdvanceConfirm) {
      return;
    }
    if (!this.hasLockedAnswer(this.curCard().qid)) {
      return;
    }
    await this.refreshCardStatus();
    if (this.cardsLeft <= 0) {
      this.showNoCard = true;
      return;
    }
    this.showUnlock = true;
  }

  /// 确认用卡：只解锁当前小题（qid）的过程与答案，不碰其它题；
  /// 同时走 /card/unlock 记账，今日已用与余额以网关为准
  private async confirmUnlock(): Promise<void> {
    if (this.cardsLeft <= 0) {
      this.showUnlock = false;
      this.showNoCard = true;
      return;
    }
    const qid: string = this.curCard().qid;
    const qk: string = qid.length > 0 ? qid : 'manual';
    this.showUnlock = false;
    this.cardLoading = true;
    try {
      if (this.cardDevId.length === 0) {
        await this.refreshCardStatus();
      }
      const dev: string = this.cardDevId.length > 0 ? this.cardDevId : 'JTP4-PHONE';
      const req = http.createHttp();
      const resp: http.HttpResponse = await req.request(this.gwBase() + '/card/unlock', {
        method: http.RequestMethod.POST,
        header: { 'Content-Type': 'application/json' },
        extraData: JSON.stringify({ device_id: dev, qkey: qk }),
        connectTimeout: 8000,
        readTimeout: 8000,
        usingCache: false,
        expectDataType: http.HttpDataType.STRING
      });
      req.destroy();
      if (resp.responseCode !== 200) {
        promptAction.showToast({ message: `用卡失败（HTTP ${resp.responseCode}），可重试` });
        return;
      }
      const b = JSON.parse(String(resp.result)) as CardPreborrowResp;
      if (!b.ok) {
        promptAction.showToast({ message: '用卡失败，可重试' });
        return;
      }
      if (b.reason === 'no_card') {
        this.cardsLeft = 0;
        this.showNoCard = true;
        return;
      }
      this.cardsLeft = typeof b.remaining === 'number' ? b.remaining : this.cardsLeft - 1;
      for (const c of this.allCards) {
        if (c.qid === qid && c.locked) {
          c.locked = false;
        }
      }
      this.saveState();
      this.refreshNav();
      for (let i = 0; i < this.navCards.length; i++) {
        if (this.navCards[i].qid === qid && this.navCards[i].kind === '过程与答案') {
          this.cardIdx = i;
          break;
        }
      }
      await this.refreshCardStatus();
    } catch (e) {
      jlog(`用卡失败: ${e}`);
      promptAction.showToast({ message: '用卡失败：网关不可达，可重试' });
    } finally {
      this.cardLoading = false;
    }
  }

  /// serverUrl 形如 http://ip:8000/solve，申请单接口在网关根地址下
  private gwBase(): string {
    const t: string = this.serverUrl.trim();
    const i: number = t.indexOf('/solve');
    if (i > 0) {
      return t.substring(0, i);
    }
    return t;
  }

  /// 点「自己来预支」先看当日上限：到上限当天不再给，只能申请或等次日
  private openAdvanceConfirm(): void {
    this.resetPreborrowIfNewDay();
    if (this.preborrowRemain() <= 0) {
      promptAction.showToast({ message: `今日预支已到上限 ${this.maxPreborrow} 张，向家长申请或等明天` });
      return;
    }
    this.showAdvanceConfirm = true;
  }

  /// 预支确认弹窗里点「确认预支」：每次 1 张、走 /card/preborrow 记账（余额可为负）。
  /// 上限为客户端实现（每日累计到 maxPreborrow 当天不再给），平台强制校验待后续同步。
  private async confirmAdvance(): Promise<void> {
    this.resetPreborrowIfNewDay();
    if (this.preborrowToday >= this.maxPreborrow) {
      this.showAdvanceConfirm = false;
      promptAction.showToast({ message: `今日预支已到上限 ${this.maxPreborrow} 张，向家长申请或等明天` });
      return;
    }
    const qid: string = this.curCard().qid;
    const qk: string = qid.length > 0 ? qid : 'manual';
    if (this.cardDevId.length === 0) {
      await this.refreshCardStatus();
    }
    const dev: string = this.cardDevId.length > 0 ? this.cardDevId : 'JTP4-PHONE';
    this.cardLoading = true;
    try {
      const req = http.createHttp();
      const resp: http.HttpResponse = await req.request(this.gwBase() + '/card/preborrow', {
        method: http.RequestMethod.POST,
        header: { 'Content-Type': 'application/json' },
        extraData: JSON.stringify({ device_id: dev, qkey: qk }),
        connectTimeout: 8000,
        readTimeout: 8000,
        usingCache: false,
        expectDataType: http.HttpDataType.STRING
      });
      req.destroy();
      if (resp.responseCode !== 200) {
        promptAction.showToast({ message: `预支失败（HTTP ${resp.responseCode}），可重试` });
        return;
      }
      const b = JSON.parse(String(resp.result)) as CardPreborrowResp;
      if (!b.ok) {
        promptAction.showToast({ message: '预支失败，可重试' });
        return;
      }
      // 网关已按“预支 1 张”解锁该题并记账：本地只同步，不再重复扣卡
      this.preborrowToday += 1;
      this.preborrowDay = this.todayStr();
      this.cardsLeft = typeof b.remaining === 'number' ? b.remaining : this.cardsLeft - 1;
      this.addRecord('advance', 1, qk);
      for (const c of this.allCards) {
        if (c.qid === qid && c.locked) {
          c.locked = false;
        }
      }
      this.showAdvanceConfirm = false;
      this.showNoCard = false;
      this.saveState();
      this.refreshNav();
      for (let i = 0; i < this.navCards.length; i++) {
        if (this.navCards[i].qid === qid && this.navCards[i].kind === '过程与答案') {
          this.cardIdx = i;
          break;
        }
      }
      promptAction.showToast({
        message: `已预支 1 张（今日 ${this.preborrowToday}/${this.maxPreborrow}，余额 ${this.cardsLeft}）`
      });
      await this.refreshCardStatus();
    } catch (e) {
      jlog(`预支失败: ${e}`);
      promptAction.showToast({ message: '预支失败：网关不可达，可重试' });
    } finally {
      this.cardLoading = false;
    }
  }

  /// 向家长申请：走 /card/apply 建申请单（一次 5 张），发完立即回思路页，不轮询。
  private async submitParentRequest(): Promise<void> {
    if (this.reqSubmitting) {
      return;
    }
    this.reqSubmitting = true;
    try {
      if (this.cardDevId.length === 0) {
        await this.refreshCardStatus();
      }
      const dev: string = this.cardDevId.length > 0 ? this.cardDevId : 'JTP4-PHONE';
      const qid: string = this.curCard().qid;
      const req = http.createHttp();
      const resp: http.HttpResponse = await req.request(this.gwBase() + '/card/apply', {
        method: http.RequestMethod.POST,
        header: { 'Content-Type': 'application/json' },
        extraData: JSON.stringify({ device_id: dev, qkey: qid.length > 0 ? qid : 'manual' }),
        connectTimeout: 8000,
        readTimeout: 8000,
        usingCache: false,
        expectDataType: http.HttpDataType.STRING
      });
      req.destroy();
      if (resp.responseCode === 200) {
        const body = JSON.parse(String(resp.result)) as CardApplyResp;
        if (body.ok && body.application_id) {
          if (body.dup) {
            // 已有待批单：不重复建单、不重复留痕
            this.pendingReqId = body.application_id;
            this.saveState();
            promptAction.showToast({ message: '已发送，等待家长批准' });
          } else {
            this.pendingReqId = body.application_id;
            this.addRecord('request', 0, body.application_id);
            this.saveState();
            promptAction.showToast({ message: `发送成功，家长在「管理 > 用卡记录」批准 5 张` });
          }
          this.showNoCard = false;
          await this.refreshCardStatus();
          return;
        }
      }
      promptAction.showToast({ message: `发送失败（HTTP ${resp.responseCode}），可重试` });
    } catch (e) {
      jlog(`提交申请失败: ${e}`);
      promptAction.showToast({ message: '发送失败：网关不可达，可重试' });
    } finally {
      this.reqSubmitting = false;
      this.showNoCard = false; // 无论成功失败，立即回思路页
    }
  }

  /// 取消申请 = 不申请了：网关 /card/cancel 把该单置为已取消，家长页不可再批；本端清待批、回思路页
  private async cancelParentRequest(): Promise<void> {
    const rid: string = this.pendingReqId;
    if (rid.length === 0) {
      this.showNoCard = false;
      return;
    }
    try {
      const req = http.createHttp();
      const resp: http.HttpResponse = await req.request(this.gwBase() + '/card/cancel', {
        method: http.RequestMethod.POST,
        header: { 'Content-Type': 'application/json' },
        extraData: JSON.stringify({ application_id: rid }),
        connectTimeout: 8000,
        readTimeout: 8000,
        usingCache: false,
        expectDataType: http.HttpDataType.STRING
      });
      req.destroy();
      if (resp.responseCode === 200) {
        this.pendingReqId = '';
        this.saveState();
        promptAction.showToast({ message: '已取消申请' });
        await this.refreshCardStatus();
      } else {
        jlog(`取消申请 HTTP ${resp.responseCode}: ${String(resp.result)}`);
        promptAction.showToast({ message: `取消失败（HTTP ${resp.responseCode}），可重试` });
      }
    } catch (e) {
      jlog(`取消申请失败: ${e}`);
      promptAction.showToast({ message: '取消失败：网关不可达，可重试' });
    } finally {
      this.showNoCard = false;
    }
  }

  /// 留痕记录按时间正序解析；JSON 损坏当空表，不炸页面
  private recordList(): RecItem[] {
    try {
      return JSON.parse(this.cardRecordsJson) as RecItem[];
    } catch (e) {
      return [];
    }
  }

  private addRecord(type: string, amount: number, reqId: string): void {
    const list: RecItem[] = this.recordList();
    const item: RecItem = { type: type, ts: Date.now(), amount: amount, reqId: reqId };
    list.push(item);
    this.cardRecordsJson = JSON.stringify(list);
  }

  private recStats(): string {
    let adv: number = 0;
    let reqN: number = 0;
    let appr: number = 0;
    for (const r of this.recordList()) {
      if (r.type === 'advance') {
        adv++;
      } else if (r.type === 'request') {
        reqN++;
      } else if (r.type === 'approved') {
        appr++;
      }
    }
    return `累计预支 ${adv} 次 · 申请 ${reqN} 次 · 批准 ${appr} 次`;
  }

  private recText(r: RecItem): string {
    if (r.type === 'advance') {
      return '预支 1 张（自己预支）';
    }
    if (r.type === 'request') {
      return '向家长申请发卡';
    }
    if (r.type === 'approved') {
      return `家长批准 +${r.amount} 张`;
    }
    return r.type;
  }

  private fmtTs(ts: number): string {
    const d: Date = new Date(ts);
    const p = (n: number): string => (n < 10 ? `0${n}` : `${n}`);
    return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`;
  }

  /// 读卡账本：不带 device_id 时 server 取最近活跃设备（单家庭账号测试版）
  private async refreshCardStatus(): Promise<void> {
    this.cardLoading = true;
    this.cardErr = '';
    try {
      const req = http.createHttp();
      const resp: http.HttpResponse = await req.request(this.gwBase() + '/card/status', {
        method: http.RequestMethod.GET,
        connectTimeout: 8000,
        readTimeout: 8000,
        usingCache: false,
        expectDataType: http.HttpDataType.STRING
      });
      req.destroy();
      if (resp.responseCode !== 200) {
        this.cardErr = `读卡账本失败（HTTP ${resp.responseCode}）`;
        return;
      }
      const b = JSON.parse(String(resp.result)) as CardStatusResp;
      this.cardDevId = b.device_id ?? '';
      this.cardBalance = b.balance ?? 0;
      this.cardQuota = b.daily_quota ?? 5;
      this.cardUsed = b.used_today ?? 0;
      this.cardPreborrow = b.preborrow_total ?? 0;
      this.cardPendingJson = JSON.stringify(b.pending_applications ?? []);
      this.cardSrvRecordsJson = JSON.stringify(b.records ?? []);
      this.resetPreborrowIfNewDay();
      // 本地待批单对账：网关上已非 pending（批准/取消/不存在）就清掉
      if (this.pendingReqId.length > 0) {
        let stillPending: boolean = false;
        for (const a of (b.pending_applications ?? [])) {
          if (a.application_id === this.pendingReqId) {
            stillPending = true;
            break;
          }
        }
        if (!stillPending) {
          this.pendingReqId = '';
        }
      }
      // server 是唯一账本：本地卡数跟 server 对齐
      this.cardsLeft = this.cardBalance;
      if (this.cardQuotaEdit.length === 0) {
        this.cardQuotaEdit = `${this.cardQuota}`;
      }
      if (this.cardMaxEdit.length === 0) {
        this.cardMaxEdit = `${this.maxPreborrow}`;
      }
      this.saveState();
      jlog(`card status: dev=${this.cardDevId} balance=${this.cardBalance} used=${this.cardUsed}`);
    } catch (e) {
      this.cardErr = '读卡账本失败：网关不可达';
      jlog(`card status 失败: ${e}`);
    } finally {
      this.cardLoading = false;
    }
  }

  /// 家长批准：一次 +5 张（server 侧加卡并记账）
  private async approveCard(aid: string): Promise<void> {
    if (this.cardLoading || aid.length === 0) {
      return;
    }
    this.cardLoading = true;
    try {
      const req = http.createHttp();
      const resp: http.HttpResponse = await req.request(this.gwBase() + '/card/approve', {
        method: http.RequestMethod.POST,
        header: { 'Content-Type': 'application/json' },
        extraData: JSON.stringify({ application_id: aid }),
        connectTimeout: 8000,
        readTimeout: 8000,
        usingCache: false,
        expectDataType: http.HttpDataType.STRING
      });
      req.destroy();
      if (resp.responseCode === 200) {
        promptAction.showToast({ message: '已批准，+5 张' });
      } else if (resp.responseCode === 409) {
        promptAction.showToast({ message: '该申请单已处理过' });
      } else {
        promptAction.showToast({ message: `批准失败（HTTP ${resp.responseCode}）` });
      }
    } catch (e) {
      jlog(`批准失败: ${e}`);
      promptAction.showToast({ message: '批准失败：网关不可达' });
    } finally {
      this.cardLoading = false;
    }
    await this.refreshCardStatus();
  }

  /// 改每日张数：server 立即按“配额 - 今日已用”重算当日余额并记账（已用不清零）
  private async saveCardQuota(): Promise<void> {
    const raw: string = this.cardQuotaEdit.length > 0 ? this.cardQuotaEdit : `${this.cardQuota}`;
    const n: number = parseInt(raw);
    if (isNaN(n) || n < 0) {
      promptAction.showToast({ message: '每日张数要填 0 或正整数' });
      return;
    }
    if (this.cardDevId.length === 0) {
      await this.refreshCardStatus();
    }
    if (this.cardDevId.length === 0) {
      promptAction.showToast({ message: '还没读到设备，先刷新用卡记录' });
      return;
    }
    this.cardLoading = true;
    try {
      const req = http.createHttp();
      const resp: http.HttpResponse = await req.request(this.gwBase() + '/card/quota', {
        method: http.RequestMethod.POST,
        header: { 'Content-Type': 'application/json' },
        extraData: JSON.stringify({ device_id: this.cardDevId, daily_quota: n }),
        connectTimeout: 8000,
        readTimeout: 8000,
        usingCache: false,
        expectDataType: http.HttpDataType.STRING
      });
      req.destroy();
      if (resp.responseCode === 200) {
        promptAction.showToast({ message: `每日张数已改成 ${n}` });
      } else {
        promptAction.showToast({ message: `改每日张数失败（HTTP ${resp.responseCode}）` });
      }
    } catch (e) {
      jlog(`改每日张数失败: ${e}`);
      promptAction.showToast({ message: '改每日张数失败：网关不可达' });
    } finally {
      this.cardLoading = false;
    }
    await this.refreshCardStatus();
  }

  /// 改设置前过「用卡记录密码」：只认 App 内密码，不用手机解锁、指纹代替。
  /// 首次没有密码时，本次输入即为设置密码（形式由家长定，不限位数、不限字符）。
  private askSettingPwd(action: string): void {
    this.pwdAction = action;
    this.pwdInput = '';
    this.pwdErr = '';
    this.pwdFirstTime = this.pwd.length === 0;
    this.showPwdGate = true;
  }

  private confirmSettingPwd(): void {
    const v: string = this.pwdInput.trim();
    if (v.length === 0) {
      this.pwdErr = '请输入用卡记录密码';
      return;
    }
    if (this.pwdFirstTime) {
      this.pwd = v;
      this.saveState();
    } else if (v !== this.pwd) {
      this.pwdErr = '密码不对，再试一次';
      return;
    }
    this.showPwdGate = false;
    const act: string = this.pwdAction;
    this.pwdAction = '';
    if (act === 'quota') {
      this.saveCardQuota();
    } else if (act === 'max') {
      this.saveMaxPreborrow();
    }
  }

  private closePwdGate(): void {
    this.showPwdGate = false;
    this.pwdAction = '';
    this.pwdInput = '';
    this.pwdErr = '';
  }

  /// 预支上限：默认每天 5 张（每日张数 1 倍），家长可改；仅本地生效。
  /// 平台待开发：网关 max_preborrow 字段与超限拒绝，待后续同步，当前不是平台强制。
  private saveMaxPreborrow(): void {
    const raw: string = this.cardMaxEdit.length > 0 ? this.cardMaxEdit : `${this.maxPreborrow}`;
    const n: number = parseInt(raw);
    if (isNaN(n) || n < 0 || n > 99) {
      promptAction.showToast({ message: '预支上限要填 0~99 的整数' });
      return;
    }
    this.maxPreborrow = n;
    this.cardMaxEdit = `${n}`;
    this.saveState();
    jlog(`maxPreborrow 改为 ${n}（客户端上限）`);
    promptAction.showToast({ message: `预支上限已改成每天 ${n} 张（客户端生效）` });
  }

  private cardPending(): CardApp[] {
    try {
      return JSON.parse(this.cardPendingJson) as CardApp[];
    } catch (e) {
      return [];
    }
  }

  private cardSrvRecords(): CardRec[] {
    try {
      return JSON.parse(this.cardSrvRecordsJson) as CardRec[];
    } catch (e) {
      return [];
    }
  }

  private cardActionText(r: CardRec): string {
    const q: string = r.qkey && r.qkey.length > 0 ? `题 ${r.qkey}` : '';
    const d: number = r.delta ?? 0;
    const ds: string = d > 0 ? `+${d}` : `${d}`;
    const a: string = r.action ?? '';
    if (a === 'unlock') {
      return `解题扣卡 ${ds} 张 · 余额 ${r.balance} · ${q}`;
    }
    if (a === 'preborrow') {
      return `自己预支 ${ds} 张 · 余额 ${r.balance} · ${q}`;
    }
    if (a === 'apply') {
      return `向家长申请 · 余额 ${r.balance} · ${q}`;
    }
    if (a === 'approve') {
      return `家长批准 ${ds} 张 · 余额 ${r.balance}`;
    }
    if (a === 'cancel') {
      return `申请已取消 · ${q}`;
    }
    if (a === 'dev_reset') {
      return `测试重置 · 余额 ${r.balance}`;
    }
    return `${a} ${ds} · 余额 ${r.balance}`;
  }

  @Builder
  TopBar() {
    Row({ space: 10 }) {
      Text('简题')
        .fontSize(34)
        .fontWeight(FontWeight.Bold)
        .fontColor('#1A1A1A')
      Text('极简作业 AI 助手')
        .fontSize(14)
        .fontColor('#8A8A8A')
        .margin({ top: 10 })
      Blank()
      Text('管理')
        .fontSize(15)
        .fontColor('#1A5FB4')
        .padding({ left: 10, right: 10, top: 8, bottom: 8 })
        .onClick(() => {
          this.adminPage = 'menu';
          this.view = 'admin';
        })
    }
    .width('100%')
    .height(56)
  }

  @Builder
  BigShootButton(size: number) {
    Button(this.busy ? '识别中…' : '拍题')
      .width(size)
      .height(size)
      .borderRadius(size / 2)
      .fontSize(Math.round(size * 0.18))
      .fontWeight(FontWeight.Bold)
      .fontColor(Color.White)
      .backgroundColor(this.busy ? '#8FA8C8' : '#1A5FB4')
      .enabled(!this.busy)
      .shadow({ radius: 18, color: '#331A5FB4', offsetY: 8 })
      .onClick(() => {
        this.captureAndSend();
      })
  }

  @Builder
  HomeView() {
    Column() {
      this.TopBar()
      if (this.isLandscape) {
        Column({ space: 12 }) {
          this.BigShootButton(200)
          Text('对准题目，按一下')
            .fontSize(14)
            .fontColor('#8A8A8A')
          if (this.busy) {
            Button('取消')
              .height(32)
              .fontSize(14)
              .fontColor(Color.White)
              .backgroundColor('#C0392B')
              .borderRadius(16)
              .padding({ left: 24, right: 24 })
              .onClick(() => {
                this.cancelRequest();
              })
          }
        }
        .layoutWeight(1)
        .width('100%')
        .justifyContent(FlexAlign.Center)
      } else {
        Column({ space: 14 }) {
          this.BigShootButton(210)
          Text('对准题目，按一下')
            .fontSize(14)
            .fontColor('#8A8A8A')
        }
        .layoutWeight(1)
        .width('100%')
        .justifyContent(FlexAlign.Center)
      }
    }
    .width('100%')
    .height('100%')
    .padding({ left: 18, right: 18, bottom: 14 })
  }

  @Builder
  ResultView() {
    if (this.isLandscape) {
      this.ResultViewLand()
    } else {
      this.ResultViewPort()
    }
  }

  /// 横屏结果页：一题一显；导航常驻（有图没图都有）；默认只走思路卡，过程与答案用卡解锁
  @Builder
  ResultViewLand() {
    Stack() {
      Column({ space: 6 }) {
        // 顶栏：返回 | 完成 | 解题(居中悬浮于顶栏，不压滚动内容) | 耗时 | 再拍
        Row({ space: 10 }) {
          Button('返回')
            .height(30)
            .fontSize(14)
            .fontColor(Color.White)
            .backgroundColor('#1A5FB4')
            .borderRadius(8)
            .onClick(() => {
              this.statusText = '待拍题';
              this.view = 'home';
            })
          Text(this.resultTitle)
            .fontSize(17)
            .fontWeight(FontWeight.Bold)
            .fontColor(this.resultTitle === '完成' ? '#1A1A1A' : '#C0392B')
          Blank()
          // 当前思路卡有锁定的过程与答案时才露「解题」（屏幕上方正中央）
          if (this.curCard().kind === '思路' && this.hasLockedAnswer(this.curCard().qid)) {
            Button('解题')
              .height(30)
              .fontSize(14)
              .fontWeight(FontWeight.Bold)
              .fontColor(Color.White)
              .backgroundColor('#C0392B')
              .borderRadius(15)
              .padding({ left: 28, right: 28 })
              .onClick(() => {
                this.tryOpenUnlock();
              })
          }
          Blank()
          Text(this.timingText.length > 0 ? `耗时 ${this.timingText}` : '')
            .fontSize(15)
            .fontWeight(FontWeight.Bold)
            .fontColor('#1A5FB4')
          Button('再拍一题')
            .height(32)
            .fontSize(14)
            .fontWeight(FontWeight.Bold)
            .fontColor('#22272E')
            .backgroundColor(Color.Transparent)
            .linearGradient({ angle: 180, colors: [['#FCFDFE', 0], ['#D9E0E8', 0.45], ['#AAB6C2', 0.55], ['#8B98A5', 1]] })
            .border({ width: 1, color: '#6E7A86' })
            .borderRadius(8)
            .enabled(!this.busy)
            .onClick(() => {
              // 先回拍摄页，由用户按拍题键发起下一次，不直接跳进识别中
              this.statusText = '待拍题';
              this.view = 'home';
            })
        }
        .width('100%')
        .height(38)

        // 主区域：图片按宽拉伸填满、超高可上下滚；文本模式同款分页。只看不点。
        Stack({ alignContent: Alignment.Top }) {
          Scroll() {
            if (this.curCard().pm !== null) {
              Image(this.curCard().pm)
                .width('100%')
                .aspectRatio(this.curCard().aspect)
                .objectFit(ImageFit.Fill)
                .borderRadius(8)
            } else {
              Text(this.curCard().text.length > 0 ? this.curCard().text : '（暂无内容）')
                .fontSize(16)
                .lineHeight(26)
                .width('100%')
                .textAlign(TextAlign.Start)
                .copyOption(CopyOptions.LocalDevice)
                .padding(12)
            }
          }
          .width('100%')
          .height('100%')
          .backgroundColor(Color.White)
          .borderRadius(12)
          .align(Alignment.TopStart)
          .scrollBar(BarState.Auto)
          .gesture(
            PanGesture({ direction: PanDirection.Horizontal })
              .onActionEnd((e: GestureEvent) => {
                if (e.offsetX < -60) {
                  this.goNext();
                } else if (e.offsetX > 60) {
                  this.goPrev();
                }
              })
          )
        }
        .layoutWeight(1)
        .width('100%')

        // 底部导航：贴底边，一题一显，n/N 按小题算
        Row({ space: 12 }) {
          Button('上一题')
            .layoutWeight(1)
            .height(44)
            .fontSize(16)
            .fontWeight(FontWeight.Medium)
            .backgroundColor(this.curQIndex() > 0 ? '#1A5FB4' : '#C7CED6')
            .enabled(this.curQIndex() > 0)
            .onClick(() => {
              this.goPrev();
            })
          Column({ space: 1 }) {
            Text(this.curCard().label)
              .fontSize(13)
              .fontColor('#885500')
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
            Text(this.navPosText())
              .fontSize(12)
              .fontColor('#8A8A8A')
          }
          .width(150)
          Button('下一题')
            .layoutWeight(1)
            .height(44)
            .fontSize(16)
            .fontWeight(FontWeight.Medium)
            .backgroundColor(this.curQIndex() >= 0 && this.curQIndex() < this.navQCount() - 1 ? '#1A5FB4' : '#C7CED6')
            .enabled(this.curQIndex() >= 0 && this.curQIndex() < this.navQCount() - 1)
            .onClick(() => {
              this.goNext();
            })
        }
        .width('100%')
        .height(48)
      }
      .width('100%')
      .height('100%')
      .padding({ left: 18, right: 18, top: 6, bottom: 4 })
      .focusable(true)
      .defaultFocus(true)
      .onKeyEvent((e: KeyEvent) => {
        // 物理确定键双击 -> 同「解题」按钮（Enter/方向键OK/音量键都收，覆盖面优先）
        if (e.type === KeyType.Down) {
          const code: number = e.keyCode;
          if (code === 66 || code === 2054 || code === 2055 || code === 2088 ||
              code === 16 || code === 17 || code === 24 || code === 25) {
            const now: number = Date.now();
            if (now - this.lastKeyTs < 450) {
              this.tryOpenUnlock();
            }
            this.lastKeyTs = now;
          }
        }
      })

      // 用卡确认弹窗
      if (this.showUnlock) {
        Column() {
          Column({ space: 14 }) {
            Text('看过程与答案')
              .fontSize(17)
              .fontWeight(FontWeight.Bold)
              .fontColor('#1A1A1A')
            Text(`今日剩余 ${this.cardsLeft} 张，是否使用 1 张看本题过程与答案？`)
              .fontSize(14)
              .fontColor('#444444')
              .textAlign(TextAlign.Center)
              .width('100%')
            Row({ space: 14 }) {
              Button('取消')
                .layoutWeight(1)
                .height(40)
                .fontSize(15)
                .backgroundColor('#C7CED6')
                .onClick(() => {
                  this.showUnlock = false;
                })
              Button('确认用卡')
                .layoutWeight(1)
                .height(40)
                .fontSize(15)
                .fontColor(Color.White)
                .backgroundColor(this.cardsLeft > 0 ? '#C0392B' : '#C7CED6')
                .enabled(this.cardsLeft > 0)
                .onClick(() => {
                  this.confirmUnlock();
                })
            }
            .width('100%')
          }
          .width(400)
          .padding(20)
          .backgroundColor(Color.White)
          .borderRadius(14)
        }
        .width('100%')
        .height('100%')
        .backgroundColor('#66000000')
        .justifyContent(FlexAlign.Center)
      }

      // 卡用完临时弹窗（只盖在思路页上，关闭/退出/重进都回思路页）：
      // 无待批单 -> 自己来预支 / 向家长申请 / 关闭；有待批单 -> 自己来预支 / 取消申请 / 关闭
      if (this.showNoCard) {
        Column() {
          Column({ space: 12 }) {
            Text('解题卡已用光')
              .fontSize(17)
              .fontWeight(FontWeight.Bold)
              .fontColor('#1A1A1A')
            if (this.pendingReqId.length === 0) {
              Text('看过程与答案需要解题卡，可以预支或向家长申请')
                .fontSize(14)
                .fontColor('#444444')
                .textAlign(TextAlign.Center)
                .width('100%')
              Button('自己来预支')
                .width('100%')
                .height(44)
                .fontSize(15)
                .fontColor(Color.White)
                .backgroundColor('#C0392B')
                .onClick(() => {
                  this.openAdvanceConfirm();
                })
              Button(this.reqSubmitting ? '发送中…' : '向家长申请')
                .width('100%')
                .height(44)
                .fontSize(15)
                .fontColor(Color.White)
                .backgroundColor('#1A5FB4')
                .enabled(!this.reqSubmitting)
                .onClick(() => {
                  this.submitParentRequest();
                })
            } else {
              Text('已发送，等待家长批准')
                .fontSize(14)
                .fontColor('#885500')
                .textAlign(TextAlign.Center)
                .width('100%')
              Text(`家长在「管理 > 用卡记录」批准 5 张`)
                .fontSize(12)
                .fontColor('#8A8A8A')
                .textAlign(TextAlign.Center)
                .width('100%')
              Button('自己来预支')
                .width('100%')
                .height(44)
                .fontSize(15)
                .fontColor(Color.White)
                .backgroundColor('#C0392B')
                .onClick(() => {
                  this.openAdvanceConfirm();
                })
              Button('取消申请')
                .width('100%')
                .height(44)
                .fontSize(15)
                .fontColor(Color.White)
                .backgroundColor('#6E7A86')
                .onClick(() => {
                  this.cancelParentRequest();
                })
            }
            Button('关闭')
              .width('100%')
              .height(44)
              .fontSize(15)
              .backgroundColor('#C7CED6')
              .onClick(() => {
                // 只关弹窗，不撤单，回思路页
                this.showNoCard = false;
              })
          }
          .width(400)
          .padding(20)
          .backgroundColor(Color.White)
          .borderRadius(14)
        }
        .width('100%')
        .height('100%')
        .backgroundColor('#66000000')
        .justifyContent(FlexAlign.Center)
      }

      // 预支确认弹窗：确认后才 +1 张、留痕
      if (this.showAdvanceConfirm) {
        Column() {
          Column({ space: 14 }) {
            Text(`预支 1 张解题卡（今日已预支 ${this.preborrowToday}/${this.maxPreborrow}）`)
              .fontSize(17)
              .fontWeight(FontWeight.Bold)
              .fontColor('#1A1A1A')
            Text('自己做的主，保护好屁股。')
              .fontSize(14)
              .fontColor('#C0392B')
              .textAlign(TextAlign.Center)
              .width('100%')
            Text('预支会留痕，家长在用卡记录里可见')
              .fontSize(13)
              .fontColor('#8A8A8A')
              .textAlign(TextAlign.Center)
              .width('100%')
            Row({ space: 14 }) {
              Button('取消')
                .layoutWeight(1)
                .height(40)
                .fontSize(15)
                .backgroundColor('#C7CED6')
                .onClick(() => {
                  this.showAdvanceConfirm = false;
                })
              Button('确认预支')
                .layoutWeight(1)
                .height(40)
                .fontSize(15)
                .fontColor(Color.White)
                .backgroundColor('#C0392B')
                .onClick(() => {
                  this.confirmAdvance();
                })
            }
            .width('100%')
          }
          .width(400)
          .padding(20)
          .backgroundColor(Color.White)
          .borderRadius(14)
        }
        .width('100%')
        .height('100%')
        .backgroundColor('#66000000')
        .justifyContent(FlexAlign.Center)
      }
    }
    .width('100%')
    .height('100%')
  }

  /// 竖屏结果页（后补，先保留旧布局）
  @Builder
  ResultViewPort() {
    Column({ space: 12 }) {
      Row({ space: 8 }) {
        Text(this.resultTitle)
          .fontSize(20)
          .fontWeight(FontWeight.Bold)
          .fontColor(this.resultTitle === '完成' ? '#1A1A1A' : '#C0392B')
        Blank()
        // 计时位：网关回了 upload_ms/answer_ms 才显示，没回就不占位
        Text(this.timingText)
          .fontSize(20)
          .fontWeight(FontWeight.Bold)
          .fontColor('#1A5FB4')
        Text('返回')
          .fontSize(15)
          .fontColor('#1A5FB4')
          .padding({ left: 10, right: 4, top: 8, bottom: 8 })
          .onClick(() => {
            this.statusText = '待拍题';
            this.view = 'home';
          })
      }
      .width('100%')
      .height(44)

      Scroll() {
        Column({ space: 10 }) {
          if (this.shots.length > 0) {
            ForEach(this.shots, (s: ShotView, idx: number) => {
              Column({ space: 4 }) {
                Text(s.label)
                  .fontSize(13)
                  .fontWeight(FontWeight.Bold)
                  .fontColor('#885500')
                  .width('100%')
                if (s.pm !== null) {
                  Image(s.pm)
                    .width('100%')
                    .objectFit(ImageFit.Contain)
                    .borderRadius(4)
                }
              }
              .width('100%')
            }, (s: ShotView, idx: number) => `${idx}_${s.label}`)
          }
          Text(this.resultText.length > 0 ? this.resultText : '（网关未返回内容）')
            .fontSize(16)
            .lineHeight(26)
            .width('100%')
            .textAlign(TextAlign.Start)
            .copyOption(CopyOptions.LocalDevice)
        }
        .width('100%')
        .alignItems(HorizontalAlign.Start)
      }
      .layoutWeight(1)
      .width('100%')
      .padding(12)
      .backgroundColor(Color.White)
      .borderRadius(12)
      .align(Alignment.TopStart)

      Button('再拍一题')
        .width('100%')
        .height(48)
        .fontSize(17)
        .fontWeight(FontWeight.Medium)
        .backgroundColor('#1A5FB4')
        .enabled(!this.busy)
        .onClick(() => {
          this.captureAndSend();
        })
    }
    .width('100%')
    .height('100%')
    .padding({ left: 18, right: 18, top: 8, bottom: 14 })
  }

  @Builder
  AdminCard(title: string, desc: string, action?: () => void) {
    Row() {
      Column({ space: 6 }) {
        Text(title)
          .fontSize(17)
          .fontWeight(FontWeight.Medium)
          .fontColor('#1A1A1A')
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .width('100%')
        Text(desc)
          .fontSize(12)
          .lineHeight(18)
          .fontColor('#8A8A8A')
          .maxLines(2)
          .width('100%')
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)

      Text('›')
        .fontSize(22)
        .fontColor('#BBBBBB')
        .margin({ left: 8 })
    }
    .width('100%')
    .height(96)
    .alignItems(VerticalAlign.Center)
    .padding({ left: 18, right: 18 })
    .backgroundColor(Color.White)
    .borderRadius(12)
    .onClick(() => {
      if (action) {
        action();
      } else {
        promptAction.showToast({ message: `${title}：接口待 server 定版后接入` });
      }
    })
  }

  @Builder
  SettingsView() {
    Column({ space: 12 }) {
      Row({ space: 8 }) {
        Text('返回')
          .fontSize(15)
          .fontColor('#1A5FB4')
          .padding({ right: 10, top: 8, bottom: 8 })
          .onClick(() => {
            this.adminPage = 'menu';
          })
        Text('设置')
          .fontSize(20)
          .fontWeight(FontWeight.Bold)
          .fontColor('#1A1A1A')
        Blank()
      }
      .width('100%')
      .height(44)

      Column({ space: 8 }) {
        Text('服务器地址')
          .fontSize(13)
          .fontColor('#666666')
          .width('100%')
        TextInput({ text: this.serverUrl, placeholder: 'http://网关地址:8000/solve' })
          .onChange((value: string) => {
            this.serverUrl = value;
            this.saveState();
          })
          .type(InputType.URL)
          .height(44)
          .fontSize(14)
          .backgroundColor('#F4F6F8')
          .borderRadius(10)
        Text('修改后立即生效')
          .fontSize(12)
          .fontColor('#8A8A8A')
          .width('100%')
      }
      .width('100%')
      .padding(14)
      .backgroundColor(Color.White)
      .borderRadius(12)

      Column({ space: 6 }) {
        Text('图像与上传参数')
          .fontSize(15)
          .fontWeight(FontWeight.Medium)
          .fontColor('#1A1A1A')
          .width('100%')
        Text('上传前自动压缩至 2MB 以内；接口联调待 server 定版后接入')
          .fontSize(13)
          .fontColor('#8A8A8A')
          .width('100%')
      }
      .width('100%')
      .padding(14)
      .backgroundColor(Color.White)
      .borderRadius(12)

      Blank()
    }
    .width('100%')
    .height('100%')
    .padding({ left: 18, right: 18, top: 8, bottom: 14 })
  }

  /// 用卡记录页（家长可见）：数据来自 server 卡账本 cards_ledger.jsonl
  @Builder
  RecordsView() {
    Stack({ alignContent: Alignment.Center }) {
      Column({ space: 10 }) {
      Row({ space: 8 }) {
        Text('返回')
          .fontSize(15)
          .fontColor('#1A5FB4')
          .padding({ right: 10, top: 8, bottom: 8 })
          .onClick(() => {
            this.adminPage = 'menu';
          })
        Text('用卡记录')
          .fontSize(20)
          .fontWeight(FontWeight.Bold)
          .fontColor('#1A1A1A')
        Blank()
        Text(this.cardLoading ? '读取中…' : '刷新')
          .fontSize(15)
          .fontColor('#1A5FB4')
          .padding({ left: 10, right: 10, top: 8, bottom: 8 })
          .onClick(() => {
            this.refreshCardStatus();
          })
      }
      .width('100%')
      .height(44)

      // 顶部：设备ID（完整）+ 余额 + 今日已用
      Column({ space: 6 }) {
        Text(`设备ID  ${this.cardDevId.length > 0 ? this.cardDevId : '（未读到）'}`)
          .fontSize(13)
          .fontColor('#444444')
          .width('100%')
        Row({ space: 16 }) {
          Text(`余额 ${this.cardBalance} 张`)
            .fontSize(16)
            .fontWeight(FontWeight.Bold)
            .fontColor('#1A5FB4')
          Text(`今日已用 ${this.cardUsed}/${this.cardQuota}`)
            .fontSize(14)
            .fontColor('#1A1A1A')
          Text(`累计预支 ${this.cardPreborrow} 张`)
            .fontSize(14)
            .fontColor('#885500')
          Blank()
        }
        .width('100%')

        // 当日预支：按自然日累计，到上限当天不再给
        Text(`当日已预支 ${this.preborrowToday} 张 · 还可预支 ${this.preborrowRemain()} 张（上限 ${this.maxPreborrow} 张/天）`)
          .fontSize(13)
          .fontColor(this.preborrowRemain() <= 0 ? '#C0392B' : '#885500')
          .width('100%')

        // 每日张数可改（改完 server 立即按“新张数 - 今日已用”重算余额，已用不清零）
        Row({ space: 8 }) {
          Text('每日张数')
            .fontSize(13)
            .fontColor('#444444')
          TextInput({ text: this.cardQuotaEdit, placeholder: `${this.cardQuota}` })
            .type(InputType.Number)
            .width(64)
            .height(32)
            .fontSize(13)
            .onChange((v: string) => {
              this.cardQuotaEdit = v;
            })
          Text('保存')
            .fontSize(13)
            .fontColor(Color.White)
            .padding({ left: 12, right: 12, top: 6, bottom: 6 })
            .backgroundColor('#1A5FB4')
            .borderRadius(12)
            .onClick(() => {
              this.askSettingPwd('quota');
            })
          Blank()
        }
        .width('100%')

        // 预支上限可改（默认 5＝每日张数 1 倍；客户端实现，平台强制校验待后续同步）
        Row({ space: 8 }) {
          Text('预支上限')
            .fontSize(13)
            .fontColor('#444444')
          TextInput({ text: this.cardMaxEdit, placeholder: `${this.maxPreborrow}` })
            .type(InputType.Number)
            .width(64)
            .height(32)
            .fontSize(13)
            .onChange((v: string) => {
              this.cardMaxEdit = v;
            })
          Text('保存')
            .fontSize(13)
            .fontColor(Color.White)
            .padding({ left: 12, right: 12, top: 6, bottom: 6 })
            .backgroundColor('#885500')
            .borderRadius(12)
            .onClick(() => {
              this.askSettingPwd('max');
            })
          Blank()
        }
        .width('100%')

        if (this.cardErr.length > 0) {
          Text(this.cardErr)
            .fontSize(12)
            .fontColor('#C0392B')
            .width('100%')
        }
      }
      .width('100%')
      .padding(14)
      .backgroundColor(Color.White)
      .borderRadius(12)
      .onAppear(() => {
        this.refreshCardStatus();
      })

      // 待批申请：家长批准一次 +5 张
      if (this.cardPending().length > 0) {
        Column({ space: 8 }) {
          Text('待批申请')
            .fontSize(14)
            .fontWeight(FontWeight.Bold)
            .fontColor('#885500')
            .width('100%')
          ForEach(this.cardPending(), (a: CardApp) => {
            Row({ space: 10 }) {
              Text(`${a.time ?? ''}  题 ${a.qkey ?? '-'}`)
                .fontSize(13)
                .fontColor('#1A1A1A')
                .layoutWeight(1)
              Text('批准5张')
                .fontSize(14)
                .fontColor(Color.White)
                .padding({ left: 14, right: 14, top: 8, bottom: 8 })
                .backgroundColor('#27AE60')
                .borderRadius(16)
                .onClick(() => {
                  this.approveCard(a.application_id ?? '');
                })
            }
            .width('100%')
            .padding({ left: 14, right: 14, top: 10, bottom: 10 })
            .backgroundColor(Color.White)
            .borderRadius(10)
          }, (a: CardApp) => `${a.application_id}`)
        }
        .width('100%')
      }

      Scroll() {
        Column({ space: 8 }) {
          if (this.cardSrvRecords().length === 0) {
            Text(this.cardErr.length > 0 ? '（读不到账本，点右上刷新重试）' : '（暂无记录）')
              .fontSize(14)
              .fontColor('#8A8A8A')
              .width('100%')
              .textAlign(TextAlign.Center)
              .margin({ top: 24 })
          }
          ForEach(this.cardSrvRecords().slice().reverse(), (r: CardRec) => {
            Row({ space: 10 }) {
              Text(r.time ?? '')
                .fontSize(12)
                .fontColor('#8A8A8A')
                .width(120)
              Text(this.cardActionText(r))
                .fontSize(14)
                .fontColor('#1A1A1A')
                .layoutWeight(1)
            }
            .width('100%')
            .padding({ left: 14, right: 14, top: 12, bottom: 12 })
            .backgroundColor(Color.White)
            .borderRadius(10)
          }, (r: CardRec) => `${r.ts}_${r.action}_${r.qkey}`)
        }
        .width('100%')
      }
      .layoutWeight(1)
      .width('100%')
    }
    .width('100%')
    .height('100%')
    .padding({ left: 18, right: 18, top: 8, bottom: 14 })
      if (this.showPwdGate) {
        this.PwdGate()
      }
    }
    .width('100%')
    .height('100%')
  }

  /// 用卡记录密码门：改每日张数 / 改预支上限前必须输入 App 内密码
  @Builder
  PwdGate() {
    Column() {
      Column({ space: 12 }) {
        Text(this.pwdFirstTime ? '首次设置用卡记录密码' : '输入用卡记录密码')
          .fontSize(17)
          .fontWeight(FontWeight.Bold)
          .fontColor('#1A1A1A')
        Text('只认这个密码，不用手机解锁或指纹')
          .fontSize(12)
          .fontColor('#8A8A8A')
        TextInput({ text: this.pwdInput, placeholder: '用卡记录密码' })
          .type(InputType.Normal)
          .height(44)
          .fontSize(16)
          .onChange((v: string) => {
            this.pwdInput = v;
            this.pwdErr = '';
          })
          .onSubmit(() => {
            this.confirmSettingPwd();
          })
        if (this.pwdErr.length > 0) {
          Text(this.pwdErr)
            .fontSize(13)
            .fontColor('#C0392B')
        }
        Row({ space: 12 }) {
          Blank()
          Text('取消')
            .fontSize(15)
            .fontColor('#666666')
            .padding({ left: 16, right: 16, top: 8, bottom: 8 })
            .onClick(() => {
              this.closePwdGate();
            })
          Text('确认')
            .fontSize(15)
            .fontColor(Color.White)
            .padding({ left: 18, right: 18, top: 8, bottom: 8 })
            .backgroundColor('#1A5FB4')
            .borderRadius(14)
            .onClick(() => {
              this.confirmSettingPwd();
            })
        }
        .width('100%')
      }
      .width(360)
      .padding(20)
      .backgroundColor(Color.White)
      .borderRadius(16)
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#88000000')
    .justifyContent(FlexAlign.Start)
    .padding({ top: 40 })
  }

  @Builder
  AdminView() {
    if (this.adminPage === 'records') {
      this.RecordsView()
    } else if (this.adminPage === 'settings') {
      this.SettingsView()
    } else {
      Column({ space: 12 }) {
        Row({ space: 8 }) {
          Text('返回')
            .fontSize(15)
            .fontColor('#1A5FB4')
            .padding({ right: 10, top: 8, bottom: 8 })
            .onClick(() => {
              this.view = 'home';
            })
          Text('管理终端')
            .fontSize(20)
            .fontWeight(FontWeight.Bold)
            .fontColor('#1A1A1A')
          Blank()
        }
        .width('100%')
        .height(44)

        if (this.isLandscape) {
          Row({ space: 12 }) {
            Column() {
              this.AdminCard('绑定', '绑定当前设备到账号')
            }
            .layoutWeight(1)
            Column() {
              this.AdminCard('设备列表', '查看已绑定终端')
            }
            .layoutWeight(1)
            Column() {
              this.AdminCard('设置', '服务器地址与上传参数', () => {
                this.adminPage = 'settings';
              })
            }
            .layoutWeight(1)
            Column() {
              this.AdminCard('用卡记录', '预支与申请留痕，家长可见', () => {
                this.adminPage = 'records';
              })
            }
            .layoutWeight(1)
          }
          .width('100%')
          .alignItems(VerticalAlign.Top)
        } else {
          Column({ space: 12 }) {
            this.AdminCard('绑定', '绑定当前设备到账号')
            this.AdminCard('设备列表', '查看已绑定终端')
            this.AdminCard('设置', '服务器地址与上传参数', () => {
              this.adminPage = 'settings';
            })
            this.AdminCard('用卡记录', '预支与申请留痕，家长可见', () => {
              this.adminPage = 'records';
            })
          }
          .width('100%')
        }

        Blank()
      }
      .width('100%')
      .height('100%')
      .padding({ left: 18, right: 18, top: 8, bottom: 14 })
    }
  }

  build() {
    Column() {
      if (this.view === 'admin') {
        this.AdminView()
      } else if (this.view === 'result') {
        this.ResultView()
      } else {
        this.HomeView()
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#F4F6F8')
    .onAreaChange((_oldValue: Area, newValue: Area) => {
      const w: number = Number(newValue.width.toString());
      const h: number = Number(newValue.height.toString());
      this.isLandscape = w > h;
    })
  }
}
