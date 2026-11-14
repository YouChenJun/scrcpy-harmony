import media from 'libscrcpy_agent.so';
@Entry
@Component
struct Index {
  @StorageLink('scrcpyStatus') status: string = '等待电脑连接';
  build() {
    Column({ space: 24 }) {
      Text('Scrcpy Harmony').fontSize(28).fontWeight(FontWeight.Bold).fontColor(Color.White)
      Text(this.status).fontSize(20).fontColor(Color.White).textAlign(TextAlign.Center)
      Text('在电脑运行 scrcpy-harmony 后，按系统提示允许屏幕共享。').fontSize(16).fontColor('#cbd5e1')
      Button('停止投屏').onClick(() => { media.stop(); this.status = '正在停止连接'; })
    }.padding(32).width('100%').height('100%').justifyContent(FlexAlign.Center).backgroundColor('#11233d')
  }
}
