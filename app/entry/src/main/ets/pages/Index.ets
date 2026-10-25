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

const DEFAULT_URL: string = 'http://192.168.31.221:8000/solve';
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

/// 网关申请单
interface ReqItem {
  id: string;
  ts: number;
  device: string;
  status: string;
  amount: number;
}

interface ReqListResp {
  ok?: boolean;
  requests?: ReqItem[];
}

interface ReqCreateResp {
  ok?: boolean;
  id?: string;
  dup?: boolean; // true=已有待批单，网关未重复建单
}

/// jt_state.json 磁盘状态
interface StateFile {
  cardsLeft?: number;
  records?: string;
  consumed?: string;
  pendingReqId?: string;
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
      jlog(`loadState cardsLeft=${this.cardsLeft} pending=${this.pendingReqId}`);
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
        pendingReqId: this.pendingReqId
      });
      const f: fs.File = fs.openSync(this.statePath(),
        fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      fs.writeSync(f.fd, body);
      fs.closeSync(f);
    } catch (e) {
      jlog(`saveState 失败: ${e}`);
    }
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
          this.checkRequests();
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
        // 每次进结果页都从网关刷新一次卡数（家长可能已批准），不只用本地缓存
        this.checkRequests();
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

  /// 导航只走思路卡 + 已解锁的过程与答案卡
  private refreshNav(): void {
    const list: Card[] = [];
    for (const c of this.allCards) {
      if (c.kind === '思路' || !c.locked) {
        list.push(c);
      }
    }
    if (list.length === 0) {
      list.push(this.emptyCard);
    }
    this.navCards = list;
    if (this.cardIdx >= list.length) {
      this.cardIdx = list.length - 1;
    }
  }

  private curCard(): Card {
    if (this.cardIdx >= 0 && this.cardIdx < this.navCards.length) {
      return this.navCards[this.cardIdx];
    }
    return this.emptyCard;
  }

  private goPrev(): void {
    if (this.cardIdx > 0) {
      this.cardIdx -= 1;
    }
  }

  private goNext(): void {
    if (this.cardIdx < this.navCards.length - 1) {
      this.cardIdx += 1;
    }
  }

  /// n/N 按小题算：同一小题的思路卡与过程卡共享一个序号，N=小题总数（不按卡数翻倍）
  private navPosText(): string {
    if (this.navCards.length === 0) {
      return '0/0';
    }
    const qids: string[] = [];
    for (const c of this.navCards) {
      if (qids.indexOf(c.qid) < 0) {
        qids.push(c.qid);
      }
    }
    const i: number = qids.indexOf(this.curCard().qid);
    return `${i >= 0 ? i + 1 : 0}/${qids.length}`;
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
    await this.checkRequests();
    if (this.cardsLeft <= 0) {
      this.showNoCard = true;
      return;
    }
    this.showUnlock = true;
  }

  /// 确认用卡：只解锁当前小题（qid）的过程与答案，不碰其它题
  private confirmUnlock(): void {
    if (this.cardsLeft <= 0) {
      this.showUnlock = false;
      this.showNoCard = true;
      return;
    }
    const qid: string = this.curCard().qid;
    for (const c of this.allCards) {
      if (c.qid === qid && c.locked) {
        c.locked = false;
      }
    }
    this.cardsLeft -= 1;
    this.saveState();
    this.refreshNav();
    for (let i = 0; i < this.navCards.length; i++) {
      if (this.navCards[i].qid === qid && this.navCards[i].kind === '过程与答案') {
        this.cardIdx = i;
        break;
      }
    }
    this.showUnlock = false;
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

  /// 预支确认弹窗里点「确认预支」：+1 张、留痕，不限次数
  private confirmAdvance(): void {
    this.cardsLeft += 1;
    this.addRecord('advance', 1, '');
    this.showAdvanceConfirm = false;
    this.showNoCard = false;
    this.saveState();
    promptAction.showToast({ message: '已预支 1 张（已留痕，家长可见）' });
  }

  /// 向家长申请：异步即时消息，发完立即回思路页，不弹等待弹窗、不轮询。
  /// 成功只 toast「发送成功」（含家长页地址）；失败 toast 可重试，不产生待批状态。
  private async submitParentRequest(): Promise<void> {
    if (this.reqSubmitting) {
      return;
    }
    this.reqSubmitting = true;
    try {
      const req = http.createHttp();
      const resp: http.HttpResponse = await req.request(this.gwBase() + '/api/parent_request', {
        method: http.RequestMethod.POST,
        header: { 'Content-Type': 'application/json' },
        extraData: '{"device":"phone"}',
        connectTimeout: 8000,
        readTimeout: 8000,
        usingCache: false,
        expectDataType: http.HttpDataType.STRING
      });
      req.destroy();
      if (resp.responseCode === 200) {
        const body = JSON.parse(String(resp.result)) as ReqCreateResp;
        if (body.ok && body.id) {
          if (body.dup) {
            // 已有待批单：不重复建单、不重复留痕
            this.pendingReqId = body.id;
            this.saveState();
            promptAction.showToast({ message: '已发送，等待家长批准' });
          } else {
            this.pendingReqId = body.id;
            this.addRecord('request', 0, body.id);
            this.saveState();
            promptAction.showToast({ message: `发送成功，家长页 ${this.gwBase()}/parent` });
          }
          this.showNoCard = false;
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

  /// 取消申请 = 不申请了：网关把该单置为已取消，家长页该单消失；本端清待批、回思路页
  private async cancelParentRequest(): Promise<void> {
    const rid: string = this.pendingReqId;
    if (rid.length === 0) {
      this.showNoCard = false;
      return;
    }
    try {
      const req = http.createHttp();
      const resp: http.HttpResponse = await req.request(this.gwBase() + '/api/parent_cancel', {
        method: http.RequestMethod.POST,
        header: { 'Content-Type': 'application/json' },
        extraData: `{"id":"${rid}"}`,
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

  /// 查申请单：已批准且未消费的加卡到账（toast「家长已批准 X 张」）；
  /// 本端待批单在网关上已消失/已取消/已批准时，清掉本地待批状态。
  private async checkRequests(): Promise<void> {
    try {
      const req = http.createHttp();
      const resp: http.HttpResponse = await req.request(this.gwBase() + '/api/parent_request', {
        method: http.RequestMethod.GET,
        connectTimeout: 8000,
        readTimeout: 8000,
        usingCache: false,
        expectDataType: http.HttpDataType.STRING
      });
      req.destroy();
      if (resp.responseCode !== 200) {
        return;
      }
      const body = JSON.parse(String(resp.result)) as ReqListResp;
      const reqs: ReqItem[] = body.requests ?? [];
      const consumed: string[] = this.consumedIds();
      let added: number = 0;
      let dirty: boolean = false;
      for (const r of reqs) {
        if (r.status === 'approved' && r.id.length > 0 && consumed.indexOf(r.id) < 0) {
          const amt: number = r.amount > 0 ? r.amount : 1;
          this.cardsLeft += amt;
          added += amt;
          consumed.push(r.id);
          this.addRecord('approved', amt, r.id);
          dirty = true;
        }
      }
      if (added > 0) {
        this.consumedJson = JSON.stringify(consumed);
      }
      // 本地待批单对账：网关上已非 pending（批准/取消/不存在）就清掉
      if (this.pendingReqId.length > 0) {
        let stillPending: boolean = false;
        for (const r of reqs) {
          if (r.id === this.pendingReqId && r.status === 'pending') {
            stillPending = true;
            break;
          }
        }
        if (!stillPending) {
          this.pendingReqId = '';
          dirty = true;
        }
      }
      if (dirty) {
        this.saveState();
      }
      if (added > 0) {
        promptAction.showToast({ message: `家长已批准 ${added} 张` });
      }
    } catch (e) {
      jlog(`查申请单失败: ${e}`);
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

  private consumedIds(): string[] {
    try {
      return JSON.parse(this.consumedJson) as string[];
    } catch (e) {
      return [];
    }
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

  @Builder
  TopBar() {
    Row({ space: 10 }) {
      Text('简题')
        .fontSize(34)
        .fontWeight(FontWeight.Bold)
        .fontColor('#1A1A1A')
      Text('极简作业AI助手')
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
            .backgroundColor(this.cardIdx > 0 ? '#1A5FB4' : '#C7CED6')
            .enabled(this.cardIdx > 0)
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
            .backgroundColor(this.cardIdx < this.navCards.length - 1 ? '#1A5FB4' : '#C7CED6')
            .enabled(this.cardIdx < this.navCards.length - 1)
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
                  this.showAdvanceConfirm = true;
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
              Text(`家长页 ${this.gwBase()}/parent`)
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
                  this.showAdvanceConfirm = true;
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
            Text('预支 1 张解题卡')
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

  /// 用卡记录页（家长可见）：预支/申请/批准全留痕，只追加不回改
  @Builder
  RecordsView() {
    Column({ space: 12 }) {
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
      }
      .width('100%')
      .height(44)

      Text(this.recStats())
        .fontSize(13)
        .fontColor('#885500')
        .width('100%')

      Scroll() {
        Column({ space: 8 }) {
          if (this.recordList().length === 0) {
            Text('（暂无记录）')
              .fontSize(14)
              .fontColor('#8A8A8A')
              .width('100%')
              .textAlign(TextAlign.Center)
              .margin({ top: 24 })
          }
          ForEach(this.recordList().slice().reverse(), (r: RecItem) => {
            Row({ space: 10 }) {
              Text(this.fmtTs(r.ts))
                .fontSize(12)
                .fontColor('#8A8A8A')
                .width(120)
              Text(this.recText(r))
                .fontSize(14)
                .fontColor('#1A1A1A')
                .layoutWeight(1)
            }
            .width('100%')
            .padding({ left: 14, right: 14, top: 12, bottom: 12 })
            .backgroundColor(Color.White)
            .borderRadius(10)
          }, (r: RecItem) => `${r.ts}_${r.type}_${r.amount}`)
        }
        .width('100%')
      }
      .layoutWeight(1)
      .width('100%')
    }
    .width('100%')
    .height('100%')
    .padding({ left: 18, right: 18, top: 8, bottom: 14 })
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
