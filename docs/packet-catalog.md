# Packet catalogue

Every message id this client knows about, with the name recovered
from the binary and (where known) what it is for.

**How the names were recovered.** The receive dispatcher is
`CNetMsg::CreateNetMsg` at `0x00f41fce` - a single `switch(msgId)`
with 470 cases. Each case calls a constructor whose body is
`*this = CMsg<Name>::vftable;`, which yields the class name. Send
ids are written by each class's `Create()` into `msg+6`; the send and
receive numbering is the same. See [packet-hooks.md](packet-hooks.md)
for the hook addresses and how to re-derive them.

**Wire format.**

```
[uint16 total_len][uint16 msg_id][protobuf body]
```

**Direction** is `C→S` (client to server only), `S→C` (server to
client only) or `both`, as observed. Ids with no direction have not
been seen on the wire yet - they come from the dispatch table alone.

Total ids: **488**.

- 470 recovered from the receive dispatch table
- 119 annotated with a friendly name and description
- 101 of the annotated ids also appear in the dispatch table; the rest are either login-server ids handled outside it or send-only ids resolved from their `Create()` site

## Annotated

| id | dir | name | class | meaning |
|----|-----|------|-------|---------|
| `0x0423` | S→C | ServerHello | - | Login step 1: server sends the encryption seed ("CMsgEncryptCode"). Seeds the client's 16-byte key table. 8 bytes. |
| `0x0665` | S→C | SessionData | - | Login step 4: 324-byte session challenge ("CMsgConnectEx") - repeated key blocks + 32-char hex session hash. Server list + key material. |
| `0x0674` | S→C | ServerNotify | - | Login step 3: pre-login acknowledgement ("CMsgPreLoginResp"). 8 bytes. |
| `0x0796` | C→S | LoginAuth | - | Login step 2: client answers with the account/auth blob ("CMsgAccountEx", 472 B). Built from res.dat + machine key; body is opaque (encrypted credentials), not protobuf. |
| `0x07D8` | S→C | UserInfo | `CMsgUserInfo` | Full user info (name, level, guild...). |
| `0x07DF` | both | TaskDialog | `CMsgTaskDialog` | Quest dialogue (npc talk window). Open/choice/close. |
| `0x07E4` | both | SessionPing | `CMsgTick` | 32-byte ping/echo pair (SEND then RECV). |
| `0x07E9` | S→C | UserSendShow | `CMsgUserSendShow` | Trade-show / send-show payload. |
| `0x07EB` | both | ExchangeShop | `CMsgExchangeShop` | Exchange shop (f1=3,f2=24702). |
| `0x07F1` | S→C | QualifyingDetailInfo | `CMsgQualifyingDetailInfo` | Qualifying/rank detail. |
| `0x07F4` | S→C | Ping | `CMsgPing` | Server ping. |
| `0x07FB` | S→C | BattlePassTaskList | `CMsgBattlePassTaskList` | Battle-pass task list. |
| `0x07FE` | S→C | Chat/SysMsg | `CMsgUserAttrib` | Chat + system message feed. Bodies carry the text. |
| `0x0805` | ? | Unknown805 | - | Not present in this capture; structural hole in the receive switch. Reserved/unused on this build. |
| `0x0809` | S→C | EntityRefresh | `CMsgMagicInfo` | Entity/magic list refresh. |
| `0x080E` | S→C | ItemInfoEx | `CMsgItemInfoEx` | Extended item info (large, 277-988 B). |
| `0x0810` | S→C | BattlePass | `CMsgBattlePass` | Battle-pass state. |
| `0x0817` | C→S | SelfState | `CMsgDaoQi` | 6-byte self-state ping (f1=1). |
| `0x081D` | S→C | HairFaceStorage | `CMsgHairFaceStorage` | Hair/face storage. |
| `0x081E` | both | BeastsInfo | `CMsgBeastsInfo` | Pet/beast info block. |
| `0x0822` | S→C | TexasPersonalInfo | `CMsgTexasPersonalInfo` | Card-game personal info. |
| `0x082A` | S→C | PresentUserData | `CMsgPresentUserData` | Present/gift user data. |
| `0x082B` | S→C | ServerList | `CMsgServerList` | Server list payload. |
| `0x082D` | S→C | InnerStrengthInfo | `CMsgInnerStrengthInfo` | Inner-strength (skill) info. |
| `0x0833` | both | Action | `CMsgAction` | Entity action (attack / cast / interact animation). Body is CMsgAction; the real sub-type is a field inside the protobuf (e.g. actionGetItemSet). |
| `0x0838` | both | Interact | `CMsgInteract` | Interact with npc/object (attack, gather, dialogue trigger). |
| `0x0846` | both | TradeBuddy | `CMsgTradeBuddy` | Trade partner info. |
| `0x0849` | both | MailList | `CMsgMailList` | Mailbox list (sender/subject encoded in the body). |
| `0x084E` | both | TaskStatus | `CMsgTaskStatus` | Quest/task status. |
| `0x084F` | S→C | WeaponSkill | `CMsgWeaponSkill` | Weapon skill rows. |
| `0x0855` | S→C | HangUp | `CMsgHangUp` | Hang-up / auto-play state. |
| `0x085B` | S→C | CompeteRank | `CMsgCompeteRank` | Competition rank. |
| `0x085C` | S→C | MailNotify | `CMsgMailNotify` | New-mail notification. |
| `0x085D` | S→C | SignIn | `CMsgSignIn` | Daily sign-in. |
| `0x0861` | S→C | EnemyInvadeOpt | `CMsgEnemyInvadeOpt` | Enemy-invade (siege) options. |
| `0x0867` | both | StatisticActive | `CMsgStatisticActive` | Activity statistic toggle. |
| `0x0869` | S→C | SubPro | `CMsgSubPro` | Sub-profession info. |
| `0x086C` | S→C | Time | `CMsgTime` | Server time. |
| `0x086D` | S→C | ItemStatus | `CMsgItemStatus` | Item status change. |
| `0x086F` | S→C | OwnKongfuBase | `CMsgOwnKongfuBase` | Kongfu (skill) base data. |
| `0x0875` | S→C | OverheadLeagueInfo | `CMsgOverheadLeagueInfo` | Overhead league nameplate info. |
| `0x088A` | S→C | Nosuch | `CMsgNosuch` | "No such" negative-response (f1=16). |
| `0x0896` | S→C | SilverTotal | `CMsgStatisticDaily` | Currency total (silver) update. |
| `0x0898` | both | Walk | `CMsgWalk` | Movement sync. SEND = client walk request (target pos); RECV = authoritative position broadcast. |
| `0x089A` | S→C | RaceTrackStatus | `CMsgRaceTrackStatus` | Race-track status. |
| `0x089B` | S→C | VipFunctionValidNotify | `CMsgVipFunctionValidNotify` | VIP feature validity. |
| `0x089D` | C→S | DxCheck | - | Machine/hardware fingerprint upload (188 B), ~14 s after login. 6 raw bytes hex-encoded; anti-cheat client attestation. |
| `0x08A3` | S→C | Flower | `CMsgFlower` | Flower/gift. |
| `0x08B2` | both | Npc | `CMsgNpc` | Npc info / interaction. |
| `0x08B3` | S→C | Data | `CMsgData` | Generic data block. |
| `0x08B7` | S→C | Instance | `CMsgInstance` | Instance (dungeon) state, f1=12. |
| `0x08B9` | S→C | CoatStorage | `CMsgCoatStorage` | Coat (mount skin) storage. |
| `0x08BA` | both | BetLevel | `CMsgBetLevel` | Bet / gamble level. |
| `0x08CA` | S→C | ActivityTask | `CMsgActivityTask` | Activity task. |
| `0x08CE` | S→C | Friend | `CMsgFriend` | Friend list / presence. |
| `0x08D1` | S→C | Peerage | `CMsgPeerage` | Noble peerage info. |
| `0x08D8` | S→C | SilverGain | `CMsgTeamAward` | Currency gain / drop (CMsgTeamAward gold split). |
| `0x08DA` | S→C | Family | `CMsgFamily` | Family/clan info. |
| `0x08DB` | S→C | SyndicateAttributeInfo | `CMsgSyndicateAttributeInfo` | Syndicate attribute info. |
| `0x08DE` | both | PlayerAttriInfo | `CMsgPlayerAttriInfo` | 172-byte state blob (f1=0xa469ee00), HMAC-tagged. |
| `0x08E0` | both | ChatText | `CMsgName` | Chat text / name block. |
| `0x08E3` | S→C | UserMonthCardInfo | `CMsgUserMonthCardInfo` | Monthly-card info. |
| `0x08EC` | S→C | GouYuAptitude | `CMsgGouYuAptitude` | Fishing aptitude. |
| `0x08F2` | S→C | Title | `CMsgTitle` | Title info. |
| `0x08F4` | S→C | EntitySnapshot | `CMsgPlayer` | Player/entity spawn + periodic full stat snapshot (id, hp, mp, pos, name...). The dense one, ~288 per session. |
| `0x08F6` | both | ZeroBuffer | `CMsgLoginNotice` | 520-byte zero-filled frame (padding / keep-alive). |
| `0x08F7` | S→C | EnemyList | `CMsgEnemyList` | Enemy list (names HuntreX, karma...). |
| `0x08F9` | S→C | SceneChat | `CMsgSceneChat` | Scene-local chat. |
| `0x0902` | S→C | EquipLock | `CMsgEquipLock` | Equipment lock state. |
| `0x0905` | S→C | - | - | (reserved) |
| `0x090B` | S→C | MailContent | `CMsgMailContent` | Mail body content. |
| `0x090C` | both | SafeHeat | `CMsgSafeHeat` | Account safe-heat (guard) status / auth token. |
| `0x090D` | S→C | TravelNotes | `CMsgTravelNotes` | Travel notes. |
| `0x091E` | S→C | CasinoInteractive | `CMsgCasinoInteractive` | Casino interaction. |
| `0x091F` | S→C | Rank | `CMsgRank` | Rank list entry. |
| `0x0922` | S→C | SynPrestige | `CMsgSynPrestige` | Syndicate prestige. |
| `0x0923` | S→C | BatchRefresh | `CMsgTaskDetailInfo` | Batched UI refresh (task / shop / etc.). |
| `0x0925` | S→C | MapInfo | `CMsgMapInfo` | Map metadata (id, name, bounds). |
| `0x092D` | S→C | CollectionStorage | `CMsgCollectionStorage` | Collection storage. |
| `0x092E` | S→C | ItemInfo | `CMsgItemInfo` | Item detail block (CMsgItemInfo), ~147 per session. |
| `0x0935` | S→C | ItemRefineRecord | `CMsgItemRefineRecord` | Refine record. |
| `0x0939` | S→C | Talk | `CMsgTalk` | Chat line (CMsgTalk). Carries speaker, channel, message. |
| `0x093A` | S→C | BattleEffectiveness | `CMsgBattleEffectiveness` | Combat effectiveness score. |
| `0x093C` | C→S | PbState f1=1 | `CMsgSdkLoginSign` | 6-byte client ack (f1=1). |
| `0x093E` | S→C | UserCityInfo | `CMsgUserCityInfo` | 516-byte player city/zone + world-state payload. |
| `0x093F` | both | UserAbilityScore | `CMsgUserAbilityScore` | Ability/attribute score block. |
| `0x0942` | both | DataUpload | `CMsgTQP` | TQP / raw data upload (111-901 B). |
| `0x0953` | S→C | ServerInfo | `CMsgServerInfo` | Compact server info / ack after world entry. |
| `0x0956` | S→C | RefineEffect | `CMsgRefineEffect` | Refine effect sync. |
| `0x0963` | S→C | ItemDrop | `CMsgNpcInfo` | Npc/player drop event (CMsgNpcInfo context), 62 per session. |
| `0x0973` | both | CombatGear | `CMsgCombatGear` | Combat gear (f1=4). |
| `0x0975` | both | OperatingAct | `CMsgOperatingAct` | Operating activity. |
| `0x0976` | S→C | EntityAttrList | `CMsgMagicEffect` | Attribute list for an entity (per-entity stat rows). |
| `0x097B` | S→C | ItemOp | `CMsgItem` | Item operation result (CMsgItem). |
| `0x097D` | C→S | AuthReply | - | Login step 5: client's 52-byte reply to the session challenge. |
| `0x0983` | both | Achievement | `CMsgAchievement` | Objective/achievement progress. |
| `0x0988` | both | Walk/MoveItem | `CMsgMapItem` | Ground-item / map-item move sync (CMsgMapItem). ~198 per side. |
| `0x0990` | S→C | GouYuInfo | `CMsgGouYuInfo` | Fishing/pet info list. |
| `0x0995` | S→C | RuneStorage | `CMsgRuneStorage` | Rune storage query reply. |
| `0x0998` | S→C | TitleStorage | `CMsgTitleStorage` | Title storage. |
| `0x099D` | S→C | LeagueOpt | `CMsgLeagueOpt` | League option. |
| `0x09AF` | S→C | PCCOPInteract | `CMsgPCCOPInteract` | Cop (offline-player) interaction. |
| `0x09CB` | S→C | GodWeapons | `CMsgGodWeapons` | God-weapon data. |
| `0x09E4` | S→C | VipConsumption | `CMsgVipConsumption` | VIP consumption record. |
| `0x09EE` | S→C | SwordAncestor | `CMsgSwordAncestor` | Sword-ancestor event. |
| `0x09F0` | S→C | MedalStorage | `CMsgMedalStorage` | Medal storage. |
| `0x09F9` | both | GlobalPointRank | `CMsgGlobalPointRank` | Global point rank (f1=4,f9=1,f10=4). |
| `0x0A2C` | both | Rune | `CMsgRune` | Rune operation (f1=2, f2=<role id>). |
| `0x0A36` | both | KeepAlive | `CMsgItemPing` | Heartbeat / item-ping tick (12 B each way). The steady keep-alive. |
| `0x80E1` | S→C | - | - | (reserved) |
| `0x8961` | S→C | - | - | (reserved) |
| `0x8981` | both | - | - | (reserved) |
| `0x91F1` | S→C | - | - | (reserved) |
| `0x91F2` | S→C | - | - | (reserved) |
| `0x92E1` | S→C | - | - | (reserved) |
| `0x9421` | both | - | - | (reserved) |
| `0x9631` | S→C | - | - | (reserved) |
| `0x9831` | both | - | - | (reserved) |
| `0x9832` | both | - | - | (reserved) |

## Recovered from the binary (no annotation yet)

These ids exist in the client's dispatch table but have not been
seen in a captured session, so only the class name is known.

| id | class |
|----|-------|
| `0x07D1` | `CMsgGuideInfo` |
| `0x07D3` | `CMsgNewShopGoods` |
| `0x07D4` | `CMsgOperatingActInfo` |
| `0x07D5` | `CMsgGlobalLottery` |
| `0x07D6` | `CMsgHandBrickInfo` |
| `0x07D7` | `CMsgInstanceInfo` |
| `0x07D9` | `CMsgActivityTaskReward` |
| `0x07DA` | `CMsgGuide` |
| `0x07DB` | `CMsgRouletteTable` |
| `0x07DC` | `CMsgAuctionDirList` |
| `0x07DD` | `CMsgSynCompete` |
| `0x07DE` | `CMsgFactionMatchWitness` |
| `0x07E1` | `CMsgCrossFlagWarAltar` |
| `0x07E3` | `CMsgSynRecruitAdvertising` |
| `0x07E5` | `CMsgSlotAction` |
| `0x07E6` | `CMsgInnerStrengthOpt` |
| `0x07E7` | `CMsgProcessComplete` |
| `0x07E8` | `CMsgUserMelter` |
| `0x07EA` | `CMsgCOPLeaveWord` |
| `0x07EC` | `CMsgAuction` |
| `0x07ED` | `CMsgTrade` |
| `0x07EF` | `CMsgRoulettePlayer` |
| `0x07F0` | `CMsgDataArray` |
| `0x07F2` | `CMsgRemotePrize` |
| `0x07F5` | `CMsgMelterOpt` |
| `0x07F6` | `CMsgSyndicate` |
| `0x07F7` | `CMsgOwnKongRank` |
| `0x07F8` | `CMsgShowHandCallAction` |
| `0x07FC` | `CMsgTaskReward` |
| `0x07FD` | `CMsgSlotResult` |
| `0x07FF` | `CMsgPokerFriendInvite` |
| `0x0800` | `CMsgSynRecruitAdvertisingList` |
| `0x0801` | `CMsgPokerFriendAction` |
| `0x0802` | `CMsgPresentList` |
| `0x0803` | `CMsgOSConnectInfo` |
| `0x0807` | `CMsgNosuchAutoHisList` |
| `0x0808` | `CMsgRedEnvelops` |
| `0x080A` | `CMsgSynForm` |
| `0x080C` | `CMsgPigeonQuery` |
| `0x080D` | `CMsgNewSynWar` |
| `0x080F` | `CMsgDetainItemInfo` |
| `0x0811` | `CMsgScreenChatSkinOpt` |
| `0x0812` | `CMsgVassalWarList` |
| `0x0813` | `CMsgSynMemberInfo` |
| `0x0814` | `CMsgMonsterTransform` |
| `0x0815` | `CMsgCardsLotteryRankList` |
| `0x0818` | `CMsgInviteTrans` |
| `0x0819` | `CMsgDisconnect` |
| `0x081B` | `CMsgCrossFlagWar` |
| `0x081C` | `CMsgLeagueRank` |
| `0x081F` | `CMsgRelationInfo` |
| `0x0820` | `CMsgShowHandLayCard` |
| `0x0821` | `CMsgQualifyingFightersList` |
| `0x0823` | `CMsgLeagueInfo` |
| `0x0824` | `CMsgItemVendue` |
| `0x0825` | `CMsgTreasureOpt` |
| `0x0829` | `CMsgExpPool` |
| `0x082C` | `CMsgExchangeShopGoods` |
| `0x082E` | `CMsgTeam` |
| `0x082F` | `CMsgRouletteInvite` |
| `0x0830` | `CMsgSealedTreasureList` |
| `0x0831` | `CMsgMelterRankList` |
| `0x0832` | `CMsgRankMemberShow` |
| `0x0834` | `CMsgCombatGearSkin` |
| `0x0836` | `CMsgTurnoverLottery` |
| `0x0837` | `CMsgTrainingVitalityScore` |
| `0x0839` | `CMsgExchangeInnerStrength` |
| `0x083B` | `CMsgStatistic` |
| `0x083C` | `CMsgBatchEquip` |
| `0x083D` | `CMsgRoulette1ArgAction` |
| `0x083F` | `CMsgTeamPKArenicScore` |
| `0x0840` | `CMsgSuperFlag` |
| `0x0841` | `CMsgProfLevUp` |
| `0x0842` | `CMsgMailOperation` |
| `0x0844` | `CMsgPackage` |
| `0x0845` | `CMsgAutoGroup` |
| `0x0847` | `CMsgPetInfo` |
| `0x0848` | `CMsgDutyMinContri` |
| `0x084A` | `CMsgUserTotalRefineLev` |
| `0x084B` | `CMsgGamblingTableInfo` |
| `0x084C` | `CMsgExchangeShopBuy` |
| `0x0850` | `CMsgMagicEffectTime` |
| `0x0851` | `CMsgNsdsSortData` |
| `0x0852` | `CMsgFamilyOccupyInfo` |
| `0x0853` | `CMsgTraining` |
| `0x0854` | `CMsgXuanBaoOpt` |
| `0x0856` | `CMsgAura` |
| `0x0857` | `CMsgSealedTreasureRank` |
| `0x0858` | `CMsgCrossSwitch` |
| `0x085A` | `CMsgTeamArenaRank` |
| `0x085E` | `CMsgUserIPInfo` |
| `0x085F` | `CMsgTrainingInfo` |
| `0x0864` | `CMsgTeamPKRankInfo` |
| `0x0865` | `CMsgElitePKArenic` |
| `0x0866` | `CMsgPKEliteMatchInfo` |
| `0x0868` | `CMsgShowHandRaceInteractive` |
| `0x086A` | `CMsgUserFreeze` |
| `0x086E` | `CMsgRouletteLatestProfitLossList` |
| `0x0870` | `CMsgHundredWeaponsOpt` |
| `0x0871` | `CMsgMonsterLive` |
| `0x0872` | `CMsgLeagueMemList` |
| `0x0873` | `CMsgShootGameOpt` |
| `0x0874` | `CMsgDominateTeamPopPkName` |
| `0x0877` | `CMsgTeamArenaYTop10List` |
| `0x0878` | `CMsgTeamArenaFightingMemberInfo` |
| `0x0879` | `CMsgShowHandKick` |
| `0x087B` | `CMsgQualifyingRank` |
| `0x087C` | `CMsgShowHandTrusteeship` |
| `0x087D` | `CMsgQuiz` |
| `0x087E` | `CMsgRaceTrackProp` |
| `0x087F` | `CMsgRelation` |
| `0x0880` | `CMsgRaceTrackPropEffect` |
| `0x0881` | `CMsgVlmScoreInfo` |
| `0x0882` | `CMsgChangeName` |
| `0x0883` | `CMsgWarFlag` |
| `0x0884` | `CMsgHWCompose` |
| `0x0885` | `CMsgItemDialog` |
| `0x0886` | `CMsgTexasInteractive` |
| `0x0887` | `CMsgOwnKongfuImproveFeedback` |
| `0x0888` | `CMsgDragonSkinRecord` |
| `0x0889` | `CMsgFruitMachineBossDmgRank` |
| `0x088B` | `CMsgNewSlotRecord` |
| `0x088C` | `CMsgQualifyingSeasonRankList` |
| `0x088D` | `CMsgGameTrend` |
| `0x088E` | `CMsgNewSlotCar` |
| `0x0890` | `CMsgPotHistory` |
| `0x0892` | `CMsgEquipRefineRank` |
| `0x0894` | `CMsgTrainingVitalityProtectInfo` |
| `0x0895` | `CMsgSynEventList` |
| `0x0897` | `CMsgTeamPopPKMatchInfo` |
| `0x0899` | `CMsgElitePKScore` |
| `0x089C` | `CMsgVipUserHandle` |
| `0x089E` | `CMsgItemRefineOpt` |
| `0x089F` | `CMsgBatchSpEffectOpt` |
| `0x08A0` | `CMsgLeagueRobOpt` |
| `0x08A1` | `CMsgDominoResult` |
| `0x08A2` | `CMsgArenicScore` |
| `0x08A4` | `CMsgFruitMachine` |
| `0x08A5` | `CMsgGamblingTablePlayerList` |
| `0x08A6` | `CMsgFruitMachineGoResult` |
| `0x08A7` | `CMsgLottery` |
| `0x08A8` | `CMsgTexasExMatchFieldList` |
| `0x08A9` | `CMsgShowHandActivePlayer` |
| `0x08AA` | `CMsgTeamArenaInteractive` |
| `0x08AC` | `CMsgGamblingTableOpt` |
| `0x08AD` | `CMsgRouletteNpcInfo` |
| `0x08AE` | `CMsg2ndPsw` |
| `0x08AF` | `CMsgSponsorInfo` |
| `0x08B0` | `CMsgLeagueImperialCourtList` |
| `0x08B4` | `CMsgFMRoundRobin` |
| `0x08B5` | `CMsgTeamMember` |
| `0x08B8` | `CMsgContribute` |
| `0x08BB` | `CMsgDragonSkin` |
| `0x08BC` | `CMsgCombatGearOpt` |
| `0x08BD` | `CMsgPokerFriendList` |
| `0x08BE` | `CMsgTeamPopPKArenicScore` |
| `0x08BF` | `CMsgTeamArenaScore` |
| `0x08C1` | `CMsgGamblingResult` |
| `0x08C2` | `CMsgBenefitsConfig` |
| `0x08C3` | `CMsgTexasExInteractive` |
| `0x08C4` | `CMsgMultiFruitMachineLobby` |
| `0x08C5` | `CMsgTeamPopPKRankInfo` |
| `0x08C6` | `CMsgSpiritInteractive` |
| `0x08C7` | `CMsgGamblingNpc` |
| `0x08C8` | `CMsgRouletteWinningNumber` |
| `0x08C9` | `CMsgPlayerNpcInfo` |
| `0x08CB` | `CMsgUserMonthCardAction` |
| `0x08CC` | `CMsgNosuchAutoList` |
| `0x08CD` | `CMsgNewSlotCarRank` |
| `0x08CF` | `CMsgTradeUserInfo` |
| `0x08D0` | `CMsgShowHandGameResult` |
| `0x08D2` | `CMsgFactionMatch` |
| `0x08D3` | `CMsgFamilyOccupy` |
| `0x08D4` | `CMsgMaterials` |
| `0x08D5` | `CMsgSyncAction` |
| `0x08D6` | `CMsgShowHandEnter` |
| `0x08D7` | `CMsgTeamRoll` |
| `0x08D9` | `CMsgTaskRewardRank` |
| `0x08DC` | `CMsgTrainingVitalityProtect` |
| `0x08DD` | `CMsgEnemyInvadeArenic` |
| `0x08DF` | `CMsgFMMatch` |
| `0x08E1` | `CMsgBattlePassTask` |
| `0x08E2` | `CMsgFlushExp` |
| `0x08E4` | `CMsgRouletteWatcherList` |
| `0x08E5` | `CMsgAllot` |
| `0x08E6` | `CMsgSynMemberList` |
| `0x08E8` | `CMsgTrainingVitalityExpiryNotify` |
| `0x08E9` | `CMsgFamilyDmg` |
| `0x08EA` | `CMsgTreasureSync` |
| `0x08EB` | `CMsgAthleteShop` |
| `0x08ED` | `CMsgQualifyingInteractive` |
| `0x08EE` | `CMsgTransportor` |
| `0x08EF` | `CMsgFruitMachineLobbyWinRank` |
| `0x08F0` | `CMsgNationality` |
| `0x08F1` | `CMsgFuse` |
| `0x08F3` | `CMsgMarketingAct` |
| `0x08F5` | `CMsgGemEmbed` |
| `0x08F8` | `CMsgTeamArenaHeroData` |
| `0x08FA` | `CMsgTenTimesLotteryReward` |
| `0x08FB` | `CMsgTrainingVitalityInfo` |
| `0x08FC` | `CMsgRedeemExp` |
| `0x08FD` | `CMsgCOPPrizeBroadcast` |
| `0x08FF` | `CMsgPlayerResult` |
| `0x0900` | `CMsgDominoTableAct` |
| `0x0901` | `CMsgLeagueToken` |
| `0x0904` | `CMsgItemRefine` |
| `0x0907` | `CMsgShootGame` |
| `0x0908` | `CMsgCrossFlagWarFlag` |
| `0x0909` | `CMsgMultiFruitMachineLobbyStatus` |
| `0x090E` | `CMsgWeather` |
| `0x0910` | `CMsgSynFormInfo` |
| `0x0911` | `CMsgPigeon` |
| `0x0912` | `CMsgGodExp` |
| `0x0913` | `CMsgLeaveWord` |
| `0x0914` | `CMsgMultiFruitMachineLobbyResult` |
| `0x0916` | `CMsgNpcInfoEX` |
| `0x0917` | `CMsgRandomDailyTask` |
| `0x0918` | `CMsgProcessGoalSchedule` |
| `0x0919` | `CMsgPromotionInfo` |
| `0x091A` | `CMsgDominateTeamName` |
| `0x091B` | `CMsgBossHarmRanking` |
| `0x0920` | `CMsgPaint` |
| `0x0921` | `CMsgFactionList` |
| `0x0924` | `CMsgCOPToyInfo` |
| `0x0927` | `CMsgVerifyCheck` |
| `0x092A` | `CMsgMeteSpecial` |
| `0x092B` | `CMsgLeagueSynList` |
| `0x092C` | `CMsgRoulettePlayerBet` |
| `0x092F` | `CMsgSyndicateApplyListSyn` |
| `0x0930` | `CMsgLeagueAllegianceList` |
| `0x0931` | `CMsgCheatingProgram` |
| `0x0932` | `CMsgPKEliteAction` |
| `0x0934` | `CMsgRouletteAction` |
| `0x0936` | `CMsgPromotionAct` |
| `0x0937` | `CMsgShowHandLostInfo` |
| `0x093B` | `CMsgInnerStrengthTotalInfo` |
| `0x093D` | `CMsgInvadeWarning` |
| `0x0940` | `CMsgChipsExpression` |
| `0x0941` | `CMsgMagicColdTime` |
| `0x0943` | `CMsgSealedTreasure` |
| `0x0944` | `CMsgSynpOffer` |
| `0x0945` | `CMsgNewSlotResult` |
| `0x0946` | `CMsgTaskAccumulate` |
| `0x0948` | `CMsgTeamPKArenic` |
| `0x0949` | `CMsgTrainingVitality` |
| `0x094A` | `CMsgProcessGoalTaskOpt` |
| `0x094B` | `CMsgBattlePassUser` |
| `0x094D` | `CMsgProcessGoalTask` |
| `0x094E` | `CMsgBattlePassRank` |
| `0x094F` | `CMsgOsShop` |
| `0x0950` | `CMsgLeagueApplyList` |
| `0x0951` | `CMsgProcessGoalQuery` |
| `0x0952` | `CMsgPCServerConfig` |
| `0x0957` | `CMsgGlobalLotteryRankList` |
| `0x0958` | `CMsgArenicWitness` |
| `0x0959` | `CMsgTokenUpdate` |
| `0x095B` | `CMsgBigLottery` |
| `0x095C` | `CMsgDetainItemUpdate` |
| `0x095D` | `CMsgLeaguePalaceGuardsList` |
| `0x095E` | `CMsgHundredWeaponsInfo` |
| `0x095F` | `CMsgAuctionItem` |
| `0x0961` | `CMsgShowHandOnlineStatus` |
| `0x0962` | `CMsgLogin` |
| `0x0964` | `CMsgEmoticons` |
| `0x0965` | `CMsgDominoLostInfo` |
| `0x0966` | `CMsgGouYuOpt` |
| `0x0967` | `CMsgNewSlotOpt` |
| `0x0968` | `CMsgVigor` |
| `0x096A` | `CMsgCrossFlagWarMerit` |
| `0x096B` | `CMsgMentorPlayer` |
| `0x096C` | `CMsgOSEnter` |
| `0x096D` | `CMsgPrirateDictLottery` |
| `0x096E` | `CMsgShowHandDealtCard` |
| `0x096F` | `CMsgMyNosuchQuery` |
| `0x0970` | `CMsgVassalWarOpt` |
| `0x0971` | `CMsgSolidify` |
| `0x0974` | `CMsgMagicCoat` |
| `0x0977` | `CMsgPresent` |
| `0x0978` | `CMsgShowHandExit` |
| `0x097C` | `CMsgOwnKongfuImproveSummaryInfo` |
| `0x097E` | `CMsgInnerWebPage` |
| `0x097F` | `CMsgGoldLeaguePoint` |
| `0x0980` | `CMsgOSAction` |
| `0x0982` | `CMsgTeamPopPKArenic` |
| `0x0984` | `CMsgTeamPKMatchInfo` |
| `0x0985` | `CMsgFruitMachineGo` |
| `0x0987` | `CMsgLeagueBeRob` |
| `0x0989` | `CMsgAuctionQuery` |
| `0x098A` | `CMsgScreenChatSkinSync` |
| `0x098B` | `CMsgPrincesWarBuilding` |
| `0x098C` | `CMsgFrontierFamilyWar` |
| `0x098D` | `CMsgSelfSynMemAwardRank` |
| `0x098E` | `CMsgNetSafeInteract` |
| `0x098F` | `CMsgLeagueOrderStatus` |
| `0x0991` | `CMsgNsdsHisData` |
| `0x0992` | `CMsgSynSkill` |
| `0x0993` | `CMsgLeagueRobList` |
| `0x0994` | `CMsgCrossFlagWarRank` |
| `0x0996` | `CMsgBeastsOpt` |
| `0x0997` | `CMsgElitePKGameRankInfo` |
| `0x0999` | `CMsgDeadMark` |
| `0x099A` | `CMsgTexasNpcInfo` |
| `0x099C` | `CMsgTeamArenaFightingTeamList` |
| `0x099F` | `CMsgNewTexasAct` |
| `0x09A2` | `CMsgNewTexasTableChip` |
| `0x09A3` | `CMsgTexasGameRecord` |
| `0x09A4` | `CMsgNewShowHandActivePlayer` |
| `0x09A5` | `CMsgNewShowHandCallAction` |
| `0x09A6` | `CMsgNewShowHandDealtCard` |
| `0x09A7` | `CMsgNewShowHandEnter` |
| `0x09A8` | `CMsgNewShowHandExit` |
| `0x09A9` | `CMsgNewShowHandGameResult` |
| `0x09AA` | `CMsgNewShowHandLayCard` |
| `0x09AB` | `CMsgNewShowHandLostInfo` |
| `0x09AC` | `CMsgNewShowHandOnlineStatus` |
| `0x09AD` | `CMsgNewShowHandData` |
| `0x09AE` | `CMsgNewShowHandFinishShowResult` |
| `0x09B0` | `CMsgPCCOPTableList` |
| `0x09B1` | `CMsgPCCOPCollectList` |
| `0x09B4` | `CMsgTexasGlobalRankYesterday` |
| `0x09C4` | `CMsgTexasGlobalRankActive` |
| `0x09C5` | `CMsgTexasGlobalRankActiveHis` |
| `0x09C8` | `CMsgDominoTableChip` |
| `0x09C9` | `CMsgCombatHeart` |
| `0x09CA` | `CMsgBigEmotion` |
| `0x09CC` | `CMsgGlobalActiveRoulette` |
| `0x09CD` | `CMsgGlobalActiveRouletteHis` |
| `0x09CE` | `CMsgGlobalRouletteRankYesterday` |
| `0x09CF` | `CMsgGlobalRouletteRankChange` |
| `0x09D0` | `CMsgVassalReflect` |
| `0x09D1` | `CMsgCoatOpt` |
| `0x09D2` | `CMsgTaskDialogNew` |
| `0x09DE` | `CMsgNewShowHandTrusteeship` |
| `0x09DF` | `CMsgNTexasMTTRunConfig` |
| `0x09E0` | `CMsgNTexasMTTAct` |
| `0x09E1` | `CMsgNTexasMTTList` |
| `0x09E2` | `CMsgNTexasMTTRank` |
| `0x09E3` | `CMsgNTexasMTTTableInfo` |
| `0x09E5` | `CMsgNTexasMTTNotify` |
| `0x09E6` | `CMsgNTexasSNGAct` |
| `0x09E7` | `CMsgNTexasSNGTableInfo` |
| `0x09E8` | `CMsgCOPTableTask` |
| `0x09E9` | `CMsgNTexasMTTRunRaceSort` |
| `0x09EA` | `CMsgUserLastRaceSignUp` |
| `0x09EB` | `CMsgUserSignUpRaceHistory` |
| `0x09ED` | `CMsgCampWar` |
| `0x09EF` | `CMsgCampTeamArenic` |
| `0x09F1` | `CMsgCampWarEvent` |
| `0x09F2` | `CMsgProcessGoalTaskNumReward` |
| `0x09FA` | `CMsgDominoPointTableResult` |
| `0x09FB` | `CMsgYuanshen` |
| `0x09FF` | `CMsgServerLottery` |
| `0x0A00` | `CMsgCrossSynWar` |
| `0x0A01` | `CMsgYuanShenArenic` |
| `0x0A02` | `CMsgInstanceDownCountEncrypt` |
| `0x0A03` | `CMsgUserMsgBoard` |
| `0x0A04` | `CMsgAuctionTimeOutList` |
| `0x0A0A` | `CMsgFriendRoom` |
| `0x0A0B` | `CMsgCopTrade` |
| `0x0A0C` | `CMsgCOPTradeUserInfo` |
| `0x0A29` | `CMsgSurvivorAction` |
| `0x0A2A` | `CMsgRedeemMoney` |
| `0x0A2B` | `CMsgYearFruit` |
| `0x0A33` | `CMsgUserCopVip` |
| `0x0A34` | `CMsgTeamRecruit` |
| `0x0A35` | `CMsgGuardCorps` |
| `0x0A37` | `CMsgSpritEncrypt` |
| `0x0A38` | `CMsgRichMine` |
| `0x0A39` | `CMsgUserFuncData` |

---

Regenerate this file with `python tools/gen_catalog.py` after
updating `tools/data/ids_recv.tsv` or `tools/data/overrides.tsv`.
