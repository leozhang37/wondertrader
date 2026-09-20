# 回测回归基线

这几个 csv 是用固定数据跑 min1 回测的输出，作为 HisDataReplayer
改造(秒线的双轨时间轴)的回归安全网。

数据和策略都是确定性的：
- 数据由 `bt_rig::write_min1_bars` 用固定种子生成
- 策略是 `bt_rig::BtTestStrategy`(5/20均线交叉)，不依赖外部策略工厂
- 字段里没有墙上时钟，全部来自回测时间轴

改造 HisDataReplayer 之后，`test_bt_baseline.min1_output_matches_baseline`
必须逐字节通过。如果输出确实应该变化(比如修了某个bug)，
用 `WT_UPDATE_BASELINE=1` 重新生成，并在提交信息里说明为什么变。
