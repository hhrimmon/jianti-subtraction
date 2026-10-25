import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(0x0000, 'JianTi', 'Ability onCreate');
  }

  onDestroy(): void {
    hilog.info(0x0000, 'JianTi', 'Ability onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    // 持久化在页面内手动读写 jt_state.json（PersistentStorage 在本机重启后回默认值，已废弃）
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(0x0000, 'JianTi', 'loadContent failed: %{public}s', JSON.stringify(err));
        return;
      }
    });
  }

  onWindowStageDestroy(): void {
    hilog.info(0x0000, 'JianTi', 'Ability onWindowStageDestroy');
  }

  onForeground(): void {
  }

  onBackground(): void {
  }
}
