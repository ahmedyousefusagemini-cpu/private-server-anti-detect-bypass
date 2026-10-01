# Conquer.exe (build 7952) - Wire Message-ID -> Name Map

Binary: `Conquer.exe`, x86, image base `0x00400000`. Analyzed with Ghidra (program `Conquer.exe`).
Goal: map the wire `msg_id` (uint16 at `packet+2`) to the `CMsg<Name>` class that handles it,
so a raw packet log can be rendered as readable names.

---

## 1. Executive summary

* The **complete receive dispatch** is the giant `switch` inside
  `CNetMsg::CreateNetMsg` = **`FUN_00f41fce`** (source `.../network/netmsgcreate.cpp`,
  path string @ `0x01722e10`, error string `"CNetMsg::CreateNetMsg Miss MsgType:%d at %s, %d"` @ `0x01722e60`).
* It contains **470 `case` labels** over the wire id plus a login branch (see §3).
* **All 470 game-server cases were resolved to a class name** by decompiling each case's
  constructor and reading the `*param_1 = CMsgXxx::vftable;` store (see §2 method). Plus 5
  login-branch ids. Total table = **475 ids**.
* Everything is asserted from decompiled code. Any id whose class could not be tied to a
  concrete ctor is called out in §6.

---

## 2. Method (how each name was proven)

1. Extract the `switch(param_1)` in `FUN_00f41fce` -> for every `case 0xNNNN:` the body has the shape:
   `FUN_<ctor>(); local_8=<unwindIdx>; FUN_010d33a4(body,param_2,param_3); local_8=-1; FUN_<process>(); break;`
2. Decompile `FUN_<ctor>` for every case. Each ctor is 4-6 lines and its first real statement is
   `*param_1 = CMsgXxx::vftable;` -> **that string is the class name** (Ghidra recovered the symbol).
3. The **wire id -> class** binding is therefore: `case 0xNNNN:` -> `FUN_<ctor>` -> `CMsgXxx`.
   The `FUN_010d33a4(...)` call is the protobuf `ParseFromArray` (decodes body).
4. Direction: the login-handshake log strings (`"Login Receive N : ..."`, `"Login Send N : ..."`)
   and the fact that this is the *receive* path (see §3) establish direction for the login ids.
   Game ids are, by construction, **server->client (received)** because `CreateNetMsg` is only
   reached from the receive pump (§3).

Evidence for a row is the (id -> ctor) pair; the companion `ctor_map.txt` in the working dir lists
every `case`-id with its ctor, and each ctor decompile is reproducible from that address.

---

## 3. Exact addresses / functions that constitute the dispatch

### 3.1 Receive path (raw socket -> typed message)

| Role | Address / symbol | Notes |
|---|---|---|
| Socket receive | `CMyClientSocket::DoReceive` = **`FUN_0127946d`** | recv len prefix + body, verifies checksum, calls GetMsgType |
| Wire-id getter | `FUN_00d0c67b` = **`GetMsgType`** | `return *(u16*)(packet+2);` |
| Receive filter | inline `if (uVar2 < 10000)` in `FUN_0127946d` | ids **>= 10000** are dropped before dispatch |
| Allocate raw msg | `FUN_01278f1f` | allocs `0x404` bytes (`FUN_0126b28e(0x404)`) then ctor |
| Raw msg ctor | `FUN_00d0c602` | sets `*p = CMyMsg::vftable` |
| CMyMsg vftable | **`0x016f6e90`** | +0=`0x00d0c61b` dtor, +4=**`0x00d0c687` GetMsgType** (`*(u16*)(this+6)`), +8=`0x00d0c676` GetTotalLen (`*(u16*)(this+4)`), +0xc=`0x00d0c641` |
| Queue push | `FUN_00423360` | pushes the CMyMsg onto the pending queue |
| **Message pump** | **`FUN_0104a1c6`** | dequeues each CMyMsg, reads `vtable+8` (len) and `vtable+4` (id), then **calls `FUN_00f41fce(id,len?)`** - this is the single call that drives the dispatch |
| (2nd caller) | `FUN_0113c488` | also calls `FUN_00f41fce` (secondary/scripted path) |

### 3.2 The dispatch itself

| Role | Address / symbol | Notes |
|---|---|---|
| **Master dispatch** | **`CNetMsg::CreateNetMsg` = `FUN_00f41fce`** | signature effectively `(int msgType, char* body, int bodyLen)` |
| Server-type selector | `FUN_01027d62` | `*(int*)(*(int*)(p+0x10)+0x1c08)` -> 3=login, 2=game, else=login-alt |
| Game-server switch | `switch(param_1)` @ `FUN_00f41fce` line 85 | **470 cases** (ids 0x7D1..0xA39 in-source order) |
| Game "other id" chain | `if (param_1<0x232a) ... 0x2329/0x2331..0x239f` | a small if-chain handled before/around the switch |
| Lua fallback | label `switchD_00f42234_caseD_7d2` | `FUN_010bea05(p)`: true iff `0x2329 <= p <= 0x270F`; then builds **`CNetMsgLua`** (`FUN_00e24cd7`, ctor) and runs `FUN_00e2836d` |
| Login branch (server type != 2/3) | literal `if`s in `FUN_00f41fce` | handles **0x41F, 0x423, 0x546, 0x665, 0x674** only |
| Miss (unhandled) | `FUN_01223336("CNetMsg::CreateNetMsg Miss MsgType:%d ...")` | hit when id is not a case and not in the Lua range |

Id bounds actually dispatched: **`0 < id < 10000`** (the `CMP ESI,0x2710; JNC` in the pump / the
`< 10000` check in DoReceive). Within `[0x2329, 0x270F]` an unlisted id still gets **`CNetMsgLua`**;
below `0x2329` an unlisted id is a hard **Miss** (logged + dropped).

### 3.3 Message-class plumbing (shared)

* Every `CMsgXxx` ctor starts with `FUN_010add85()` (the `CNetMsg` base ctor) then stores
  `*param_1 = CMsgXxx::vftable;`, then calls `FUN_00d0c68c()` (buffer/base init).
* `CMsgAction::Process` = `FUN_00db7125` (`msgaction.cpp`): switches on
  `*(uint*)(param_1+0x45c)` = the **action sub-type** (a field *inside* the CMsgAction body, e.g.
  0x1C3=451, 0x79=121), **not** the wire id. (Included so you don't confuse action sub-types with
  wire ids.)
* Login handshake direction (log strings, for orientation):
  * `Login Send 1` `CMsgAccountPoker`/`CMsgAccountByQRCode`/`CMsgAccountEx` @ `0x01733904/24/48`
  * `Login Receive 1` **`CMsgEncryptCode`** @ `0x0173d060`
  * `Login Send 2` `CMsgConnect` @ `0x01720bc8`
  * `Login Receive 2` **`CMsgConnectEx`** @ `0x01720b7c`

---

## 4. High-interest ids (the list you asked to prioritise)

All resolved except the four flagged in §6:

| wire id | class | macro meaning |
|---|---|---|
| 0x0423 | `CMsgEncryptCode` | login: server encryption/key code (Receive 1) |
| 0x0665 | `CMsgConnectEx` | login: connect reply (Receive 2) |
| 0x0674 | `CMsgPreLoginResp` | login: pre-login response |
| 0x0796 | **UNRESOLVED** | not in any receive switch (see §6) |
| 0x097D | **UNRESOLVED** | not in any receive switch (see §6) |
| 0x0939 | `CMsgTalk` | chat / talk |
| 0x0953 | `CMsgServerInfo` | server info |
| 0x093E | `CMsgUserCityInfo` | user city / position info |
| 0x089D | **UNRESOLVED** | not in any receive switch (see §6) |
| 0x07FE | `CMsgUserAttrib` | user attribute update |
| 0x0833 | `CMsgAction` | entity action (switch on sub-type @ +0x45C) |
| 0x0838 | `CMsgInteract` | interact (npc/obj) |
| 0x0898 | `CMsgWalk` | movement/walk |
| 0x08F4 | `CMsgPlayer` | player spawn/update |
| 0x0988 | `CMsgMapItem` | map ground item |
| 0x092E | `CMsgItemInfo` | item info |
| 0x0976 | `CMsgMagicEffect` | magic/skill effect |
| 0x0923 | `CMsgTaskDetailInfo` | task/quest detail |
| 0x0809 | `CMsgMagicInfo` | magic/skill list entry |
| 0x0896 | `CMsgStatisticDaily` | daily statistic |
| 0x08D8 | `CMsgTeamAward` | team reward |
| 0x0A36 | `CMsgItemPing` | item ping/duration |
| 0x07E4 | `CMsgTick` | server tick/heartbeat |
| 0x082B | `CMsgServerList` | server list |
| 0x0963 | `CMsgNpcInfo` | npc info |
| 0x091F | `CMsgRank` | ranking |
| 0x08E0 | `CMsgName` | name change/info |
| 0x097B | `CMsgItem` | item op |
| 0x0983 | `CMsgAchievement` | achievement |
| 0x08BA | `CMsgBetLevel` | bet/gamble level |
| 0x090C | `CMsgSafeHeat` | safe-heat (guard/heat) |
| 0x0855 | `CMsgHangUp` | hang-up/auto |
| 0x0805 | **UNRESOLVED** | not in any receive switch (see §6) |
| 0x084E | `CMsgTaskStatus` | task status |
| 0x0867 | `CMsgStatisticActive` | activity statistic |
| 0x0817 | `CMsgDaoQi` | "dao qi" (spirit/qi system) |
| 0x0869 | `CMsgSubPro` | sub-profession |
| 0x0956 | `CMsgRefineEffect` | refine effect |
| 0x0925 | `CMsgMapInfo` | map info |
| 0x08B2 | `CMsgNpc` | npc |

> Note: an earlier draft listed 0x0925/0x08B2 transposed; the ctor decompiles show
> `0x0925 -> FUN_00e2430a -> CMsgMapInfo` and `0x08B2 -> FUN_00f32a4f -> CMsgNpc`.
> The table below is authoritative.

---

## 5. Full id -> class table (475 ids)

Format: `| wire id | CMsg class | ctor function |`.
The ctor function is the address you can decompile to re-derive the class name.
All ids in this table are **received (server -> client)** and are handled by `FUN_00f41fce`,
except the 5 login ids 0x41F/0x423/0x546/0x665/0x674 (login branch, also received).

| wire id | class | ctor |
|---|---|---|
| 0x041F | `CMsgConnectOtherErr` | FUN_00ddf1a5 |
| 0x0423 | `CMsgEncryptCode` | FUN_010accf2 |
| 0x0546 | `CMsgGameServerShutDown` | FUN_00e69036 |
| 0x0665 | `CMsgConnectEx` | FUN_00f32257 |
| 0x0674 | `CMsgPreLoginResp` | FUN_00fbdbf4 |
| 0x07D1 | `CMsgGuideInfo` | FUN_0101b1aa |
| 0x07D3 | `CMsgNewShopGoods` | FUN_00e24390 |
| 0x07D4 | `CMsgOperatingActInfo` | FUN_011e23fa |
| 0x07D5 | `CMsgGlobalLottery` | FUN_010aced7 |
| 0x07D6 | `CMsgHandBrickInfo` | FUN_00ea716c |
| 0x07D7 | `CMsgInstanceInfo` | FUN_010ad06c |
| 0x07D8 | `CMsgUserInfo` | FUN_00e699e2 |
| 0x07D9 | `CMsgActivityTaskReward` | FUN_011e1882 |
| 0x07DA | `CMsgGuide` | FUN_00e2414d |
| 0x07DB | `CMsgRouletteTable` | FUN_0105c1f7 |
| 0x07DC | `CMsgAuctionDirList` | FUN_00e68c02 |
| 0x07DD | `CMsgSynCompete` | FUN_00f32d20 |
| 0x07DE | `CMsgFactionMatchWitness` | FUN_0105b939 |
| 0x07DF | `CMsgTaskDialog` | FUN_0101b69f |
| 0x07E1 | `CMsgCrossFlagWarAltar` | FUN_0105b754 |
| 0x07E3 | `CMsgSynRecruitAdvertising` | FUN_00e6989b |
| 0x07E4 | `CMsgTick` | FUN_00f805f0 |
| 0x07E5 | `CMsgSlotAction` | FUN_00d840e3 |
| 0x07E6 | `CMsgInnerStrengthOpt` | FUN_00fbd8aa |
| 0x07E7 | `CMsgProcessComplete` | FUN_00f80108 |
| 0x07E8 | `CMsgUserMelter` | FUN_00fbe11e |
| 0x07E9 | `CMsgUserSendShow` | FUN_00f32e9b |
| 0x07EA | `CMsgCOPLeaveWord` | FUN_00ef00cd |
| 0x07EB | `CMsgExchangeShop` | FUN_011e1cbb |
| 0x07EC | `CMsgAuction` | FUN_00ea6a64 |
| 0x07ED | `CMsgTrade` | FUN_0101b738 |
| 0x07EF | `CMsgRoulettePlayer` | FUN_0105c17f |
| 0x07F0 | `CMsgDataArray` | FUN_00ddf1da |
| 0x07F1 | `CMsgQualifyingDetailInfo` | FUN_00e24664 |
| 0x07F2 | `CMsgRemotePrize` | FUN_00e247f1 |
| 0x07F4 | `CMsgPing` | FUN_00d83f99 |
| 0x07F5 | `CMsgMelterOpt` | FUN_00e691da |
| 0x07F6 | `CMsgSyndicate` | FUN_00ddfd90 |
| 0x07F7 | `CMsgOwnKongRank` | FUN_0105be16 |
| 0x07F8 | `CMsgShowHandCallAction` | FUN_0101b5eb |
| 0x07FB | `CMsgBattlePassTaskList` | FUN_010f3d4e |
| 0x07FC | `CMsgTaskReward` | FUN_00e698d7 |
| 0x07FD | `CMsgSlotResult` | FUN_011e2580 |
| 0x07FE | `CMsgUserAttrib` | FUN_0101b7be |
| 0x07FF | `CMsgPokerFriendInvite` | FUN_00e69585 |
| 0x0800 | `CMsgSynRecruitAdvertisingList` | FUN_0116925e |
| 0x0801 | `CMsgPokerFriendAction` | FUN_00ea74fa |
| 0x0802 | `CMsgPresentList` | FUN_011a10c0 |
| 0x0803 | `CMsgOSConnectInfo` | FUN_010f42f2 |
| 0x0807 | `CMsgNosuchAutoHisList` | FUN_011a0f5b |
| 0x0808 | `CMsgRedEnvelops` | FUN_00f80410 |
| 0x0809 | `CMsgMagicInfo` | FUN_00f7fdf6 |
| 0x080A | `CMsgSynForm` | FUN_011e25bc |
| 0x080C | `CMsgPigeonQuery` | FUN_00ef0998 |
| 0x080D | `CMsgNewSynWar` | FUN_0101b403 |
| 0x080E | `CMsgItemInfoEx` | FUN_010f4030 |
| 0x080F | `CMsgDetainItemInfo` | FUN_00ef01f3 |
| 0x0810 | `CMsgBattlePass` | FUN_00ef003a |
| 0x0811 | `CMsgScreenChatSkinOpt` | FUN_00fbdca3 |
| 0x0812 | `CMsgVassalWarList` | FUN_00ef0d1f |
| 0x0813 | `CMsgSynMemberInfo` | FUN_00e24946 |
| 0x0814 | `CMsgMonsterTransform` | FUN_010ad316 |
| 0x0815 | `CMsgCardsLotteryRankList` | FUN_011e1b8c |
| 0x0817 | `CMsgDaoQi` | FUN_010acbd2 |
| 0x0818 | `CMsgInviteTrans` | FUN_00f7fd7e |
| 0x0819 | `CMsgDisconnect` | FUN_011e1c7f |
| 0x081B | `CMsgCrossFlagWar` | FUN_0101b05a |
| 0x081C | `CMsgLeagueRank` | FUN_010f4115 |
| 0x081D | `CMsgHairFaceStorage` | FUN_0105bace |
| 0x081E | `CMsgBeastsInfo` | FUN_0101adde |
| 0x081F | `CMsgRelationInfo` | FUN_010f43db |
| 0x0820 | `CMsgShowHandLayCard` | FUN_011a1210 |
| 0x0821 | `CMsgQualifyingFightersList` | FUN_00e246a0 |
| 0x0822 | `CMsgTexasPersonalInfo` | FUN_00ddff4c |
| 0x0823 | `CMsgLeagueInfo` | FUN_00ddf52e |
| 0x0824 | `CMsgItemVendue` | FUN_00e690cf |
| 0x0825 | `CMsgTreasureOpt` | FUN_00f8062c |
| 0x0829 | `CMsgExpPool` | FUN_010f3ee2 |
| 0x082A | `CMsgPresentUserData` | FUN_010ad790 |
| 0x082B | `CMsgServerList` | FUN_00d8404d |
| 0x082C | `CMsgExchangeShopGoods` | FUN_011e1d93 |
| 0x082D | `CMsgInnerStrengthInfo` | FUN_00fbd86e |
| 0x082E | `CMsgTeam` | FUN_011a1378 |
| 0x082F | `CMsgRouletteInvite` | FUN_0105c0cb |
| 0x0830 | `CMsgSealedTreasureList` | FUN_00f32c0a |
| 0x0831 | `CMsgMelterRankList` | FUN_00e692b4 |
| 0x0832 | `CMsgRankMemberShow` | FUN_00f32b74 |
| 0x0833 | `CMsgAction` | FUN_00d833f3 |
| 0x0834 | `CMsgCombatGearSkin` | FUN_00fbd2b5 |
| 0x0836 | `CMsgTurnoverLottery` | FUN_011e2889 |
| 0x0837 | `CMsgTrainingVitalityScore` | FUN_00f32dd4 |
| 0x0838 | `CMsgInteract` | FUN_00f32679 |
| 0x0839 | `CMsgExchangeInnerStrength` | FUN_00fbd3c3 |
| 0x083B | `CMsgStatistic` | FUN_00ea7853 |
| 0x083C | `CMsgBatchEquip` | FUN_00f31d6b |
| 0x083D | `CMsgRoulette1ArgAction` | FUN_0105c013 |
| 0x083F | `CMsgTeamPKArenicScore` | FUN_00ef0c2c |
| 0x0840 | `CMsgSuperFlag` | FUN_011a12a6 |
| 0x0841 | `CMsgProfLevUp` | FUN_00ef0a5c |
| 0x0842 | `CMsgMailOperation` | FUN_010f4151 |
| 0x0844 | `CMsgPackage` | FUN_00ea73d4 |
| 0x0845 | `CMsgAutoGroup` | FUN_010acb5a |
| 0x0846 | `CMsgTradeBuddy` | FUN_0105c4d2 |
| 0x0847 | `CMsgPetInfo` | FUN_00ddf920 |
| 0x0848 | `CMsgDutyMinContri` | FUN_00ea6d7f |
| 0x0849 | `CMsgMailList` | FUN_00d83d20 |
| 0x084A | `CMsgUserTotalRefineLev` | FUN_00ddffca |
| 0x084B | `CMsgGamblingTableInfo` | FUN_00ea6f98 |
| 0x084C | `CMsgExchangeShopBuy` | FUN_011e1cfd |
| 0x084E | `CMsgTaskStatus` | FUN_00fbdefb |
| 0x084F | `CMsgWeaponSkill` | FUN_011695f1 |
| 0x0850 | `CMsgMagicEffectTime` | FUN_00ddf56a |
| 0x0851 | `CMsgNsdsSortData` | FUN_00ef0956 |
| 0x0852 | `CMsgFamilyOccupyInfo` | FUN_00fbd449 |
| 0x0853 | `CMsgTraining` | FUN_00e699a6 |
| 0x0854 | `CMsgXuanBaoOpt` | FUN_011a1778 |
| 0x0855 | `CMsgHangUp` | FUN_00e69072 |
| 0x0856 | `CMsgAura` | FUN_011a05a7 |
| 0x0857 | `CMsgSealedTreasureRank` | FUN_00ddfcbe |
| 0x0858 | `CMsgCrossSwitch` | FUN_00fbd34b |
| 0x085A | `CMsgTeamArenaRank` | FUN_010adb56 |
| 0x085B | `CMsgCompeteRank` | FUN_010acb96 |
| 0x085C | `CMsgMailNotify` | FUN_011e222d |
| 0x085D | `CMsgSignIn` | FUN_00ef0bb4 |
| 0x085E | `CMsgUserIPInfo` | FUN_011695b5 |
| 0x085F | `CMsgTrainingInfo` | FUN_01169579 |
| 0x0861 | `CMsgEnemyInvadeOpt` | FUN_00ddf3ef |
| 0x0864 | `CMsgTeamPKRankInfo` | FUN_011e26d8 |
| 0x0865 | `CMsgElitePKArenic` | FUN_00e68f74 |
| 0x0866 | `CMsgPKEliteMatchInfo` | FUN_010f436a |
| 0x0867 | `CMsgStatisticActive` | FUN_00e69805 |
| 0x0868 | `CMsgShowHandRaceInteractive` | FUN_00ddfd54 |
| 0x0869 | `CMsgSubPro` | FUN_00fbddfb |
| 0x086A | `CMsgUserFreeze` | FUN_00ea7bc3 |
| 0x086C | `CMsgTime` | FUN_00ef0c68 |
| 0x086D | `CMsgItemStatus` | FUN_00ea71c9 |
| 0x086E | `CMsgRouletteLatestProfitLossList` | FUN_0105c107 |
| 0x086F | `CMsgOwnKongfuBase` | FUN_0105be52 |
| 0x0870 | `CMsgHundredWeaponsOpt` | FUN_01168d30 |
| 0x0871 | `CMsgMonsterLive` | FUN_011a0d64 |
| 0x0872 | `CMsgLeagueMemList` | FUN_00f7fdba |
| 0x0873 | `CMsgShootGameOpt` | FUN_010f448a |
| 0x0874 | `CMsgDominateTeamPopPkName` | FUN_00e2408d |
| 0x0875 | `CMsgOverheadLeagueInfo` | FUN_0101b4dd |
| 0x0877 | `CMsgTeamArenaYTop10List` | FUN_010adbce |
| 0x0878 | `CMsgTeamArenaFightingMemberInfo` | FUN_010ada66 |
| 0x0879 | `CMsgShowHandKick` | FUN_00fbddbf |
| 0x087B | `CMsgQualifyingRank` | FUN_00e24718 |
| 0x087C | `CMsgShowHandTrusteeship` | FUN_00f804e2 |
| 0x087D | `CMsgQuiz` | FUN_00ea757e |
| 0x087E | `CMsgRaceTrackProp` | FUN_00e696f7 |
| 0x087F | `CMsgRelation` | FUN_011e24ae |
| 0x0880 | `CMsgRaceTrackPropEffect` | FUN_01169045 |
| 0x0881 | `CMsgVlmScoreInfo` | FUN_0105c54a |
| 0x0882 | `CMsgChangeName` | FUN_00d836b0 |
| 0x0883 | `CMsgWarFlag` | FUN_00e69aaa |
| 0x0884 | `CMsgHWCompose` | FUN_01168bc0 |
| 0x0885 | `CMsgItemDialog` | FUN_00d83c3d |
| 0x0886 | `CMsgTexasInteractive` | FUN_010adc0a |
| 0x0887 | `CMsgOwnKongfuImproveFeedback` | FUN_0105beaf |
| 0x0888 | `CMsgDragonSkinRecord` | FUN_0105b8a3 |
| 0x0889 | `CMsgFruitMachineBossDmgRank` | FUN_011e1eb1 |
| 0x088A | `CMsgNosuch` | FUN_00fbdacc |
| 0x088B | `CMsgNewSlotRecord` | FUN_010f4230 |
| 0x088C | `CMsgQualifyingSeasonRankList` | FUN_00e24754 |
| 0x088D | `CMsgGameTrend` | FUN_00ea70d9 |
| 0x088E | `CMsgNewSlotCar` | FUN_00ef07f6 |
| 0x0890 | `CMsgPotHistory` | FUN_00ea753c |
| 0x0892 | `CMsgEquipRefineRank` | FUN_00d83728 |
| 0x0894 | `CMsgTrainingVitalityProtectInfo` | FUN_010adcbe |
| 0x0895 | `CMsgSynEventList` | FUN_00d8411f |
| 0x0896 | `CMsgStatisticDaily` | FUN_01169222 |
| 0x0897 | `CMsgTeamPopPKMatchInfo` | FUN_0101b6fc |
| 0x0898 | `CMsgWalk` | FUN_00ea7d1a |
| 0x0899 | `CMsgElitePKScore` | FUN_00fbd387 |
| 0x089A | `CMsgRaceTrackStatus` | FUN_00fbdc67 |
| 0x089B | `CMsgVipFunctionValidNotify` | FUN_00e24ba1 |
| 0x089C | `CMsgVipUserHandle` | FUN_0101b923 |
| 0x089E | `CMsgItemRefineOpt` | FUN_0101b207 |
| 0x089F | `CMsgBatchSpEffectOpt` | FUN_00ddf10f |
| 0x08A0 | `CMsgLeagueRobOpt` | FUN_00e24230 |
| 0x08A1 | `CMsgDominoResult` | FUN_00ddf258 |
| 0x08A2 | `CMsgArenicScore` | FUN_00e23e6c |
| 0x08A3 | `CMsgFlower` | FUN_011a0877 |
| 0x08A4 | `CMsgFruitMachine` | FUN_00d837be |
| 0x08A5 | `CMsgGamblingTablePlayerList` | FUN_00ea701c |
| 0x08A6 | `CMsgFruitMachineGoResult` | FUN_00e2410b |
| 0x08A7 | `CMsgLottery` | FUN_00fbd95e |
| 0x08A8 | `CMsgTexasExMatchFieldList` | FUN_01169407 |
| 0x08A9 | `CMsgShowHandActivePlayer` | FUN_00e24877 |
| 0x08AA | `CMsgTeamArenaInteractive` | FUN_010adb1a |
| 0x08AC | `CMsgGamblingTableOpt` | FUN_00ea6fda |
| 0x08AD | `CMsgRouletteNpcInfo` | FUN_0105c143 |
| 0x08AE | `CMsg2ndPsw` | FUN_00f31d2f |
| 0x08AF | `CMsgSponsorInfo` | FUN_0105c351 |
| 0x08B0 | `CMsgLeagueImperialCourtList` | FUN_00f327e1 |
| 0x08B2 | `CMsgNpc` | FUN_00f32a4f |
| 0x08B3 | `CMsgData` | FUN_00f323c7 |
| 0x08B4 | `CMsgFMRoundRobin` | FUN_00ddf47a |
| 0x08B5 | `CMsgTeamMember` | FUN_00ddfe31 |
| 0x08B7 | `CMsgInstance` | FUN_010acfa4 |
| 0x08B8 | `CMsgContribute` | FUN_00f7fbc5 |
| 0x08B9 | `CMsgCoatStorage` | FUN_0105b6ae |
| 0x08BA | `CMsgBetLevel` | FUN_00e68cdc |
| 0x08BB | `CMsgDragonSkin` | FUN_0101b096 |
| 0x08BC | `CMsgCombatGearOpt` | FUN_0116899c |
| 0x08BD | `CMsgPokerFriendList` | FUN_01169003 |
| 0x08BE | `CMsgTeamPopPKArenicScore` | FUN_00e24a00 |
| 0x08BF | `CMsgTeamArenaScore` | FUN_010adb92 |
| 0x08C1 | `CMsgGamblingResult` | FUN_00ea6ef2 |
| 0x08C2 | `CMsgBenefitsConfig` | FUN_0105b5ea |
| 0x08C3 | `CMsgTexasExInteractive` | FUN_01169374 |
| 0x08C4 | `CMsgMultiFruitMachineLobby` | FUN_00f328b3 |
| 0x08C5 | `CMsgTeamPopPKRankInfo` | FUN_0105c496 |
| 0x08C6 | `CMsgSpiritInteractive` | FUN_00e248b3 |
| 0x08C7 | `CMsgGamblingNpc` | FUN_00ea6e18 |
| 0x08C8 | `CMsgRouletteWinningNumber` | FUN_0105c26f |
| 0x08C9 | `CMsgPlayerNpcInfo` | FUN_0105bf63 |
| 0x08CA | `CMsgActivityTask` | FUN_011e1846 |
| 0x08CB | `CMsgUserMonthCardAction` | FUN_011a16f0 |
| 0x08CC | `CMsgNosuchAutoList` | FUN_010ad5e2 |
| 0x08CD | `CMsgNewSlotCarRank` | FUN_00d83f1b |
| 0x08CE | `CMsgFriend` | FUN_00f7fc9a |
| 0x08CF | `CMsgTradeUserInfo` | FUN_00ea7b3d |
| 0x08D0 | `CMsgShowHandGameResult` | FUN_011691e6 |
| 0x08D1 | `CMsgPeerage` | FUN_00f32ad5 |
| 0x08D2 | `CMsgFactionMatch` | FUN_0101b12c |
| 0x08D3 | `CMsgFamilyOccupy` | FUN_01168b48 |
| 0x08D4 | `CMsgMaterials` | FUN_00ea72b6 |
| 0x08D5 | `CMsgSyncAction` | FUN_010ad9d3 |
| 0x08D6 | `CMsgShowHandEnter` | FUN_00ea7794 |
| 0x08D7 | `CMsgTeamRoll` | FUN_00ea7b01 |
| 0x08D8 | `CMsgTeamAward` | FUN_00f32d5c |
| 0x08D9 | `CMsgTaskRewardRank` | FUN_0116929a |
| 0x08DA | `CMsgFamily` | FUN_00ea6dbb |
| 0x08DB | `CMsgSyndicateAttributeInfo` | FUN_00ef0bf0 |
| 0x08DC | `CMsgTrainingVitalityProtect` | FUN_010adc82 |
| 0x08DD | `CMsgEnemyInvadeArenic` | FUN_00f3251c |
| 0x08DE | `CMsgPlayerAttriInfo` | FUN_0101b573 |
| 0x08DF | `CMsgFMMatch` | FUN_00f325a7 |
| 0x08E0 | `CMsgName` | FUN_00d83e85 |
| 0x08E1 | `CMsgBattlePassTask` | FUN_011e18be |
| 0x08E2 | `CMsgFlushExp` | FUN_010acd2e |
| 0x08E3 | `CMsgUserMonthCardInfo` | FUN_010adcfa |
| 0x08E4 | `CMsgRouletteWatcherList` | FUN_0105c233 |
| 0x08E5 | `CMsgAllot` | FUN_00e23e30 |
| 0x08E6 | `CMsgSynMemberList` | FUN_0101b663 |
| 0x08E8 | `CMsgTrainingVitalityExpiryNotify` | FUN_010adc46 |
| 0x08E9 | `CMsgFamilyDmg` | FUN_00e68fb0 |
| 0x08EA | `CMsgTreasureSync` | FUN_00ef0ce8 |
| 0x08EB | `CMsgAthleteShop` | FUN_0105b564 |
| 0x08EC | `CMsgGouYuAptitude` | FUN_00d83bb7 |
| 0x08ED | `CMsgQualifyingInteractive` | FUN_00e246dc |
| 0x08EE | `CMsgTransportor` | FUN_00ddff8e |
| 0x08EF | `CMsgFruitMachineLobbyWinRank` | FUN_0101b168 |
| 0x08F0 | `CMsgNationality` | FUN_011e2338 |
| 0x08F1 | `CMsgFuse` | FUN_0105b975 |
| 0x08F2 | `CMsgTitle` | FUN_00d841f1 |
| 0x08F3 | `CMsgMarketingAct` | FUN_0105bc80 |
| 0x08F4 | `CMsgPlayer` | FUN_00e245a5 |
| 0x08F5 | `CMsgGemEmbed` | FUN_01168b84 |
| 0x08F6 | `CMsgLoginNotice` | FUN_01168d72 |
| 0x08F7 | `CMsgEnemyList` | FUN_00f7fc5e |
| 0x08F8 | `CMsgTeamArenaHeroData` | FUN_010adade |
| 0x08F9 | `CMsgSceneChat` | FUN_01169150 |
| 0x08FA | `CMsgTenTimesLotteryReward` | FUN_00e69913 |
| 0x08FB | `CMsgTrainingVitalityInfo` | FUN_011a1621 |
| 0x08FC | `CMsgRedeemExp` | FUN_00ddf9f2 |
| 0x08FD | `CMsgCOPPrizeBroadcast` | FUN_011e1951 |
| 0x08FF | `CMsgPlayerResult` | FUN_00ea7464 |
| 0x0900 | `CMsgDominoTableAct` | FUN_00ddf29a |
| 0x0901 | `CMsgLeagueToken` | FUN_0105bbfa |
| 0x0902 | `CMsgEquipLock` | FUN_00ef02c5 |
| 0x0904 | `CMsgItemRefine` | FUN_00e241aa |
| 0x0907 | `CMsgShootGame` | FUN_011e24ea |
| 0x0908 | `CMsgCrossFlagWarFlag` | FUN_00ea6d43 |
| 0x0909 | `CMsgMultiFruitMachineLobbyStatus` | FUN_00f7fe89 |
| 0x090B | `CMsgMailContent` | FUN_00ef055f |
| 0x090C | `CMsgSafeHeat` | FUN_00e69787 |
| 0x090D | `CMsgTravelNotes` | FUN_011e27ee |
| 0x090E | `CMsgWeather` | FUN_00fbe1a4 |
| 0x0910 | `CMsgSynFormInfo` | FUN_010f4643 |
| 0x0911 | `CMsgPigeon` | FUN_00f800ab |
| 0x0912 | `CMsgGodExp` | FUN_00ddf4b6 |
| 0x0913 | `CMsgLeaveWord` | FUN_00ea7259 |
| 0x0914 | `CMsgMultiFruitMachineLobbyResult` | FUN_00ddf6da |
| 0x0916 | `CMsgNpcInfoEX` | FUN_00f80015 |
| 0x0917 | `CMsgRandomDailyTask` | FUN_011a10f7 |
| 0x0918 | `CMsgProcessGoalSchedule` | FUN_00f802be |
| 0x0919 | `CMsgPromotionInfo` | FUN_011e2472 |
| 0x091A | `CMsgDominateTeamName` | FUN_010f3ea6 |
| 0x091B | `CMsgBossHarmRanking` | FUN_00f7fb83 |
| 0x091E | `CMsgCasinoInteractive` | FUN_00ef0153 |
| 0x091F | `CMsgRank` | FUN_010ad80b |
| 0x0920 | `CMsgPaint` | FUN_00e694c1 |
| 0x0921 | `CMsgFactionList` | FUN_00e240c9 |
| 0x0922 | `CMsgSynPrestige` | FUN_00ea792d |
| 0x0923 | `CMsgTaskDetailInfo` | FUN_00e249be |
| 0x0924 | `CMsgCOPToyInfo` | FUN_00e23f82 |
| 0x0925 | `CMsgMapInfo` | FUN_00e2430a |
| 0x0927 | `CMsgVerifyCheck` | FUN_011e28cb |
| 0x092A | `CMsgMeteSpecial` | FUN_00fbd99a |
| 0x092B | `CMsgLeagueSynList` | FUN_0101b29a |
| 0x092C | `CMsgRoulettePlayerBet` | FUN_0105c1bb |
| 0x092D | `CMsgCollectionStorage` | FUN_00ea6c15 |
| 0x092E | `CMsgItemInfo` | FUN_011e1fdd |
| 0x092F | `CMsgSyndicateApplyListSyn` | FUN_00f8051e |
| 0x0930 | `CMsgLeagueAllegianceList` | FUN_00fbd922 |
| 0x0931 | `CMsgCheatingProgram` | FUN_011e1c22 |
| 0x0932 | `CMsgPKEliteAction` | FUN_011e2436 |
| 0x0934 | `CMsgRouletteAction` | FUN_0105c04f |
| 0x0935 | `CMsgItemRefineRecord` | FUN_0105bb64 |
| 0x0936 | `CMsgPromotionAct` | FUN_00d83fd5 |
| 0x0937 | `CMsgShowHandLostInfo` | FUN_010ad927 |
| 0x0939 | `CMsgTalk` | FUN_011e2642 |
| 0x093A | `CMsgBattleEffectiveness` | FUN_00f7faed |
| 0x093B | `CMsgInnerStrengthTotalInfo` | FUN_00fbd8e6 |
| 0x093C | `CMsgSdkLoginSign` | FUN_011a118a |
| 0x093D | `CMsgInvadeWarning` | FUN_00ddf4f2 |
| 0x093E | `CMsgUserCityInfo` | FUN_0105c50e |
| 0x093F | `CMsgUserAbilityScore` | FUN_00e24a3c |
| 0x0940 | `CMsgChipsExpression` | FUN_00ea6b3b |
| 0x0941 | `CMsgMagicColdTime` | FUN_010ad280 |
| 0x0942 | `CMsgTQP` | FUN_00e24982 |
| 0x0943 | `CMsgSealedTreasure` | FUN_010ad8a1 |
| 0x0944 | `CMsgSynpOffer` | FUN_0105c418 |
| 0x0945 | `CMsgNewSlotResult` | FUN_00e2442b |
| 0x0946 | `CMsgTaskAccumulate` | FUN_010f46d9 |
| 0x0948 | `CMsgTeamPKArenic` | FUN_00f805b4 |
| 0x0949 | `CMsgTrainingVitality` | FUN_00fbe0e2 |
| 0x094A | `CMsgProcessGoalTaskOpt` | FUN_00f803d4 |
| 0x094B | `CMsgBattlePassUser` | FUN_00e23ea8 |
| 0x094D | `CMsgProcessGoalTask` | FUN_00f80398 |
| 0x094E | `CMsgBattlePassRank` | FUN_00d83495 |
| 0x094F | `CMsgOsShop` | FUN_00e2451f |
| 0x0950 | `CMsgLeagueApplyList` | FUN_011a0c59 |
| 0x0951 | `CMsgProcessGoalQuery` | FUN_00f801e2 |
| 0x0952 | `CMsgPCServerConfig` | FUN_010ad678 |
| 0x0953 | `CMsgServerInfo` | FUN_00ef0b78 |
| 0x0956 | `CMsgRefineEffect` | FUN_00ef0af2 |
| 0x0957 | `CMsgGlobalLotteryRankList` | FUN_00f325e3 |
| 0x0958 | `CMsgArenicWitness` | FUN_0101ada2 |
| 0x0959 | `CMsgTokenUpdate` | FUN_010f4751 |
| 0x095B | `CMsgBigLottery` | FUN_00fbcfec |
| 0x095C | `CMsgDetainItemUpdate` | FUN_00d836ec |
| 0x095D | `CMsgLeaguePalaceGuardsList` | FUN_00ef0523 |
| 0x095E | `CMsgHundredWeaponsInfo` | FUN_01168cee |
| 0x095F | `CMsgAuctionItem` | FUN_01168729 |
| 0x0961 | `CMsgShowHandOnlineStatus` | FUN_00f32ce4 |
| 0x0962 | `CMsgLogin` | FUN_00e6915a |
| 0x0963 | `CMsgNpcInfo` | FUN_00ddf89a |
| 0x0964 | `CMsgEmoticons` | FUN_011a079d |
| 0x0965 | `CMsgDominoLostInfo` | FUN_00ddf216 |
| 0x0966 | `CMsgGouYuOpt` | FUN_010f3faa |
| 0x0967 | `CMsgNewSlotOpt` | FUN_011e2374 |
| 0x0968 | `CMsgVigor` | FUN_010f4874 |
| 0x096A | `CMsgCrossFlagWarMerit` | FUN_00e68f38 |
| 0x096B | `CMsgMentorPlayer` | FUN_01168dae |
| 0x096C | `CMsgOSEnter` | FUN_010f432e |
| 0x096D | `CMsgPrirateDictLottery` | FUN_00ddf95c |
| 0x096E | `CMsgShowHandDealtCard` | FUN_0105c2ab |
| 0x096F | `CMsgMyNosuchQuery` | FUN_00ef059b |
| 0x0970 | `CMsgVassalWarOpt` | FUN_00d8422d |
| 0x0971 | `CMsgSolidify` | FUN_010f4520 |
| 0x0973 | `CMsgCombatGear` | FUN_00e68d62 |
| 0x0974 | `CMsgMagicCoat` | FUN_011a0d28 |
| 0x0975 | `CMsgOperatingAct` | FUN_00d83f5d |
| 0x0976 | `CMsgMagicEffect` | FUN_00f3281d |
| 0x0977 | `CMsgPresent` | FUN_00fbdc30 |
| 0x0978 | `CMsgShowHandExit` | FUN_00e697c9 |
| 0x097B | `CMsgItem` | FUN_00ef044b |
| 0x097C | `CMsgOwnKongfuImproveSummaryInfo` | FUN_0105beeb |
| 0x097E | `CMsgInnerWebPage` | FUN_011a0c22 |
| 0x097F | `CMsgGoldLeaguePoint` | FUN_00ef03c5 |
| 0x0980 | `CMsgOSAction` | FUN_010f42b6 |
| 0x0982 | `CMsgTeamPopPKArenic` | FUN_010f4715 |
| 0x0983 | `CMsgAchievement` | FUN_00eefffe |
| 0x0984 | `CMsgTeamPKMatchInfo` | FUN_00d841b5 |
| 0x0985 | `CMsgFruitMachineGo` | FUN_010f3f68 |
| 0x0987 | `CMsgLeagueBeRob` | FUN_010ad1a6 |
| 0x0988 | `CMsgMapItem` | FUN_0101b2d6 |
| 0x0989 | `CMsgAuctionQuery` | FUN_00fbcfb0 |
| 0x098A | `CMsgScreenChatSkinSync` | FUN_00fbdd29 |
| 0x098B | `CMsgPrincesWarBuilding` | FUN_00f32b32 |
| 0x098C | `CMsgFrontierFamilyWar` | FUN_00ef0301 |
| 0x098D | `CMsgSelfSynMemAwardRank` | FUN_00f804a6 |
| 0x098E | `CMsgNetSafeInteract` | FUN_010f418d |
| 0x098F | `CMsgLeagueOrderStatus` | FUN_011e2079 |
| 0x0990 | `CMsgGouYuInfo` | FUN_011e1f47 |
| 0x0991 | `CMsgNsdsHisData` | FUN_00ef0914 |
| 0x0992 | `CMsgSynSkill` | FUN_011a12e2 |
| 0x0993 | `CMsgLeagueRobList` | FUN_011e2197 |
| 0x0994 | `CMsgCrossFlagWarRank` | FUN_01168a32 |
| 0x0995 | `CMsgRuneStorage` | FUN_00ea75db |
| 0x0996 | `CMsgBeastsOpt` | FUN_0101ae64 |
| 0x0997 | `CMsgElitePKGameRankInfo` | FUN_01168b0c |
| 0x0998 | `CMsgTitleStorage` | FUN_011e2758 |
| 0x0999 | `CMsgDeadMark` | FUN_00f7fc22 |
| 0x099A | `CMsgTexasNpcInfo` | FUN_00f32d98 |
| 0x099C | `CMsgTeamArenaFightingTeamList` | FUN_010adaa2 |
| 0x099D | `CMsgLeagueOpt` | FUN_00d83cc3 |
| 0x099F | `CMsgNewTexasAct` | FUN_0105bd81 |
| 0x09A2 | `CMsgNewTexasTableChip` | FUN_00e693f2 |
| 0x09A3 | `CMsgTexasGameRecord` | FUN_00fbdf37 |
| 0x09A4 | `CMsgNewShowHandActivePlayer` | FUN_0101b388 |
| 0x09A5 | `CMsgNewShowHandCallAction` | FUN_0105bd06 |
| 0x09A6 | `CMsgNewShowHandDealtCard` | FUN_00e6934a |
| 0x09A7 | `CMsgNewShowHandEnter` | FUN_01168eb9 |
| 0x09A8 | `CMsgNewShowHandExit` | FUN_00fbd9d6 |
| 0x09A9 | `CMsgNewShowHandGameResult` | FUN_010ad46b |
| 0x09AA | `CMsgNewShowHandLayCard` | FUN_00f329c4 |
| 0x09AB | `CMsgNewShowHandLostInfo` | FUN_00ddf7d7 |
| 0x09AC | `CMsgNewShowHandOnlineStatus` | FUN_00f7ff9a |
| 0x09AD | `CMsgNewShowHandData` | FUN_00ea734c |
| 0x09AE | `CMsgNewShowHandFinishShowResult` | FUN_011a0ed0 |
| 0x09AF | `CMsgPCCOPInteract` | FUN_00fbdb6c |
| 0x09B0 | `CMsgPCCOPTableList` | FUN_011a0ff1 |
| 0x09B1 | `CMsgPCCOPCollectList` | FUN_01168f34 |
| 0x09B4 | `CMsgTexasGlobalRankYesterday` | FUN_011a1552 |
| 0x09C4 | `CMsgTexasGlobalRankActive` | FUN_011a13b4 |
| 0x09C5 | `CMsgTexasGlobalRankActiveHis` | FUN_011a142f |
| 0x09C8 | `CMsgDominoTableChip` | FUN_0105b7d4 |
| 0x09C9 | `CMsgCombatHeart` | FUN_011a062d |
| 0x09CA | `CMsgBigEmotion` | FUN_01168847 |
| 0x09CB | `CMsgGodWeapons` | FUN_00f7fcd6 |
| 0x09CC | `CMsgGlobalActiveRoulette` | FUN_00fbd518 |
| 0x09CD | `CMsgGlobalActiveRouletteHis` | FUN_00fbd5d7 |
| 0x09CE | `CMsgGlobalRouletteRankYesterday` | FUN_00fbd837 |
| 0x09CF | `CMsgGlobalRouletteRankChange` | FUN_00fbd800 |
| 0x09D0 | `CMsgVassalReflect` | FUN_00f806eb |
| 0x09D1 | `CMsgCoatOpt` | FUN_0101af80 |
| 0x09D2 | `CMsgTaskDialogNew` | FUN_0105c454 |
| 0x09DE | `CMsgNewShowHandTrusteeship` | FUN_00fbda51 |
| 0x09DF | `CMsgNTexasMTTRunConfig` | FUN_00ddf7a0 |
| 0x09E0 | `CMsgNTexasMTTAct` | FUN_011a0df7 |
| 0x09E1 | `CMsgNTexasMTTList` | FUN_010ad39c |
| 0x09E2 | `CMsgNTexasMTTRank` | FUN_00f328f5 |
| 0x09E3 | `CMsgNTexasMTTTableInfo` | FUN_00f7fecb |
| 0x09E4 | `CMsgVipConsumption` | FUN_00e69a68 |
| 0x09E5 | `CMsgNTexasMTTNotify` | FUN_011e2269 |
| 0x09E6 | `CMsgNTexasSNGAct` | FUN_00ef0761 |
| 0x09E7 | `CMsgNTexasSNGTableInfo` | FUN_00d83db6 |
| 0x09E8 | `CMsgCOPTableTask` | FUN_011688cd |
| 0x09E9 | `CMsgNTexasMTTRunRaceSort` | FUN_01168dea |
| 0x09EA | `CMsgUserLastRaceSignUp` | FUN_00e24ad2 |
| 0x09EB | `CMsgUserSignUpRaceHistory` | FUN_0101b854 |
| 0x09ED | `CMsgCampWar` | FUN_00ea6aa0 |
| 0x09EE | `CMsgSwordAncestor` | FUN_0105c38d |
| 0x09EF | `CMsgCampTeamArenic` | FUN_00fbd10a |
| 0x09F0 | `CMsgMedalStorage` | FUN_00ddf644 |
| 0x09F1 | `CMsgCampWarEvent` | FUN_00f32164 |
| 0x09F2 | `CMsgProcessGoalTaskNumReward` | FUN_00ef09d4 |
| 0x09F9 | `CMsgGlobalPointRank` | FUN_011a0a90 |
| 0x09FA | `CMsgDominoPointTableResult` | FUN_00ef022f |
| 0x09FB | `CMsgYuanshen` | FUN_00e24c82 |
| 0x09FF | `CMsgServerLottery` | FUN_00ea7661 |
| 0x0A00 | `CMsgCrossSynWar` | FUN_00f32293 |
| 0x0A01 | `CMsgYuanShenArenic` | FUN_010f4943 |
| 0x0A02 | `CMsgInstanceDownCountEncrypt` | FUN_010acfe6 |
| 0x0A03 | `CMsgUserMsgBoard` | FUN_00ea7c3e |
| 0x0A04 | `CMsgAuctionTimeOutList` | FUN_011687bf |
| 0x0A0A | `CMsgFriendRoom` | FUN_011a08b3 |
| 0x0A0B | `CMsgCopTrade` | FUN_010f3e64 |
| 0x0A0C | `CMsgCOPTradeUserInfo` | FUN_0101aefa |
| 0x0A29 | `CMsgSurvivorAction` | FUN_00fbde37 |
| 0x0A2A | `CMsgRedeemMoney` | FUN_00ddfacc |
| 0x0A2B | `CMsgYearFruit` | FUN_00d842c8 |
| 0x0A2C | `CMsgRune` | FUN_00ddfb9b |
| 0x0A33 | `CMsgUserCopVip` | FUN_011a16ae |
| 0x0A34 | `CMsgTeamRecruit` | FUN_00ddfe6d |
| 0x0A35 | `CMsgGuardCorps` | FUN_010acf6d |
| 0x0A36 | `CMsgItemPing` | FUN_00ef04e7 |
| 0x0A37 | `CMsgSpritEncrypt` | FUN_010f45bd |
| 0x0A38 | `CMsgRichMine` | FUN_01169081 |
| 0x0A39 | `CMsgUserFuncData` | FUN_00f32e10 |

---

## 6. Ids that could NOT be resolved, and why

The following four ids from the priority list are **not handled anywhere in the receive dispatch**:

| wire id | status |
|---|---|
| **0x0796** | not a `case` in `FUN_00f41fce`; below 0x2329 so not Lua-routed |
| **0x097D** | same |
| **0x089D** | same |
| **0x0805** | same |

Rigorous basis (all verified in the decompile):

* The `switch(param_1)` in `FUN_00f41fce` enumerates its `case` labels; the set of present ids has
  **structural gaps** exactly at these values:
  * near 0x0805: cases jump `0x0803 -> 0x0807` (0x0804, **0x0805**, 0x0806 absent)
  * near 0x089D: cases jump `0x089C -> 0x089E` (**0x089D** absent)
  * near 0x097D: cases jump `0x097C -> 0x097E` (**0x097D** absent)
  * near 0x0796: no 0x079x cases at all (range starts at 0x07D1)
* All four are `< 0x2329`, so the fallback `FUN_010bea05` (which would have produced a generic
  `CNetMsgLua`) returns **false** for them -> they would hit the `"Miss MsgType"` branch and be
  dropped, i.e. **the client has no receiver for them**.
* They are therefore almost certainly **send-only** message types (client -> server), or ids used by
  a *different build/server type*. The send-side type constants live in the outbound
  encoder/`CMsg::GetType` path, which is a separate code path from `CreateNetMsg`; confirming the
  exact class for each would require resolving that path (not done here - see "not done" below).

### Not resolved / not attempted (be explicit)

* **Send-only path**: the ids above and all other client->server types. The outbound encoder was
  not decompiled; only the receive dispatcher is covered.
* **Lua-dispatched ids `[0x2329,0x270F]`** (~1100 possible) are handled generically as `CNetMsgLua`
  by `FUN_010bea05`/`FUN_00e24cd7`; individual ids in that range have **no per-id CMsg class** in this
  binary (their handling is data/Lua driven), so they are intentionally not listed as names.
* **0x41F/0x423/0x546/0x665/0x674** appear only in the login branch (server type != 2). Their ctors
  are resolved (see table) but they are *not* `case` labels, so a naive grep of the switch misses them.

---

## 7. Confidence / provenance

* `CMsgXxx` names are **directly read** from `*param_1 = CMsgXxx::vftable;` in each ctor (Ghidra
  symbol), not guessed. Confidence: HIGH for the id->ctor->class binding.
* The **meaning** column in §4 is a plain-English gloss from the class name (e.g. `CMsgWalk` =
  movement). Treat meanings as INFERRED-from-name; nothing beyond the name was asserted.
* Direction for game ids is **received** by construction (only reached from the receive pump
  `FUN_0104a1c6`); login ids carry the explicit "Receive/Send" log evidence in §3.3.
* The four §6 ids are asserted **absent** from the receive dispatch with the gap evidence shown.

### Reproduce in Ghidra
1. `FUN_00f41fce` -> read `switch(param_1)`.
2. For a `case 0xNNNN`, take the `FUN_<ctor>` it calls, decompile it, read the `CMsgXxx::vftable` line.
3. Cross-check with the companion `ctor_map.txt` (id -> ctor) produced from the same switch.
