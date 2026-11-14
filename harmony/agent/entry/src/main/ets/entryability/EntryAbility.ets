import UIAbility from '@ohos.app.ability.UIAbility';
import Want from '@ohos.app.ability.Want';
import window from '@ohos.window';
import abilityAccessCtrl from '@ohos.abilityAccessCtrl';
import { Permissions } from '@ohos.abilityAccessCtrl';
import { BusinessError } from '@ohos.base';
import media from 'libscrcpy_agent.so';
import backgroundTaskManager from '@ohos.resourceschedule.backgroundTaskManager';
import wantAgent from '@ohos.app.ability.wantAgent';
import fs from '@ohos.file.fs';
interface MediaSession { args: Array<string>; }
export default class EntryAbility extends UIAbility {
  private args: Array<string> = [];
  private ready: boolean = false;
  private background: boolean = false;
  onCreate(want: Want): void {
    media.setLogFile(this.context.filesDir + '/scrcpy-media.log');
    AppStorage.setOrCreate('scrcpyStatus', '等待电脑连接');
    media.setReadyCallback((): void => {
      AppStorage.setOrCreate('scrcpyStatus', '正在投屏，可以返回其他应用');
    });
    media.setEndCallback((code: number): void => {
      if (media.isRunning()) { return; }
      this.stopBackground();
      AppStorage.setOrCreate('scrcpyStatus', code === 0 ? '连接已结束' : '媒体连接异常结束：' + code.toString());
    });
    media.setLauncher((name: string, display: number): void => {
      const split = name.indexOf('/');
      if (split < 1) { return; }
      this.context.startAbility({ bundleName: name.substring(0, split), abilityName: name.substring(split + 1) },
        { displayId: display }).catch((error: BusinessError) => {
          AppStorage.setOrCreate('scrcpyStatus', '打开应用失败：' + error.code.toString());
        });
    });
    this.acceptWant(want);
  }
  onNewWant(want: Want): void { this.acceptWant(want); }
  private acceptWant(want: Want): void {
    if (want.parameters) {
      const value = want.parameters['scrcpySession'];
      if (typeof value === 'string' && /^[a-f0-9]{32}$/.test(value)) {
        const path = this.context.filesDir + '/scrcpy-sessions/' + value + '.json';
        try {
          const session = JSON.parse(fs.readTextSync(path)) as MediaSession;
          fs.unlinkSync(path);
          this.args = session.args;
        } catch { AppStorage.setOrCreate('scrcpyStatus', '电脑会话不可用，请重新连接'); return; }
      }
    }
    if (this.ready && this.args.length > 0) { this.connect(); }
  }
  onWindowStageCreate(stage: window.WindowStage): void {
    stage.loadContent('pages/Index').then(() => {
      this.ready = true;
      if (this.args.length > 0) { this.connect(); }
    }).catch((error: BusinessError) => {
      AppStorage.setOrCreate('scrcpyStatus', '界面初始化失败：' + error.code.toString());
    });
  }
  private async connect(): Promise<void> {
    const permissions: Array<Permissions> = [];
    if (this.args.indexOf('video_source=camera') >= 0) { permissions.push('ohos.permission.CAMERA'); }
    if (this.args.indexOf('audio_source=mic') >= 0 && this.args.indexOf('audio=false') < 0) {
      permissions.push('ohos.permission.MICROPHONE');
    }
    try {
      if (!this.background) {
        const notification = await wantAgent.getWantAgent({
          wants: [{ bundleName: 'org.scrcpy.harmony.agent', abilityName: 'EntryAbility' }],
          operationType: wantAgent.OperationType.START_ABILITY, requestCode: 0,
          wantAgentFlags: [wantAgent.WantAgentFlags.UPDATE_PRESENT_FLAG]
        });
        await backgroundTaskManager.startBackgroundRunning(this.context,
          backgroundTaskManager.BackgroundMode.DATA_TRANSFER, notification);
        this.background = true;
      }
      if (permissions.length > 0) {
        await abilityAccessCtrl.createAtManager().requestPermissionsFromUser(this.context, permissions);
      }
      const started: boolean = media.start(this.args);
      this.args = [];
      AppStorage.setOrCreate('scrcpyStatus', started ? '正在连接电脑，请完成系统授权' : '已有连接正在运行');
    } catch (error) {
      const failure = error as BusinessError;
      AppStorage.setOrCreate('scrcpyStatus', '无法开始连接：' + failure.code.toString());
    }
  }
  private stopBackground(): void {
    if (!this.background) { return; }
    this.background = false;
    backgroundTaskManager.stopBackgroundRunning(this.context).catch((error: BusinessError) => {
      console.warn('scrcpy background cleanup: ' + error.code.toString());
    });
  }
  onDestroy(): void { media.stop(); this.stopBackground(); }
}
