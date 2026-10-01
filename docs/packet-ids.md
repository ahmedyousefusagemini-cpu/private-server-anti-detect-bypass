# Packet ID reference — decoded from `packets.log`

6870 packets, 169 distinct (direction, id) pairs.

Envelope is `[uint16 total_len][uint16 msg_id]`; the body is Protocol
Buffers. Below, `fN` is the protobuf field number, `V` varint, `L`
length-delimited (string or nested message), `F` fixed-width.

## Opening sequence (login handshake)

```
02:14:55.681  RECV 0x0423 len=8     (non-protobuf / opaque)
02:14:57.056  SEND 0x0796 len=472   f2571974644978=0x4df697a3
02:14:57.231  RECV 0x0674 len=8     (non-protobuf / opaque)
02:14:57.283  RECV 0x0665 len=324   (non-protobuf / opaque)
02:14:57.633  SEND 0x097D len=52    (non-protobuf / opaque)
02:14:57.936  RECV 0x0939 len=55    f1=16777215, f2=2101, f3=0, f4=1613, f6=0, f7=0, f8=1, f9=0
02:14:57.936  RECV 0x0953 len=12    (non-protobuf / opaque)
02:14:57.936  RECV 0x093E len=516   (non-protobuf / opaque)
02:15:11.929  SEND 0x089D len=188   (non-protobuf / opaque)
02:18:32.314  RECV 0x0423 len=8     (non-protobuf / opaque)
02:18:32.651  SEND 0x0796 len=472   f5025199=0x6ae2e180c600f59f
02:18:32.826  RECV 0x0674 len=8     (non-protobuf / opaque)
02:18:32.929  RECV 0x0665 len=324   f830=0xb11eedba, f11=3
02:18:33.460  SEND 0x097D len=52    (non-protobuf / opaque)
```

## All message IDs

### 0x0423  (1059)   2 packets

- **RECV** x2  size 8 B
  - (opaque / non-protobuf)

### 0x0665  (1637)   2 packets

- **RECV** x2  size 324 B
  - (opaque / non-protobuf)

### 0x0674  (1652)   2 packets

- **RECV** x2  size 8 B
  - (opaque / non-protobuf)

### 0x0796  (1942)   2 packets

- **SEND** x2  size 472 B
  - `f2571974644978=0x4df697a3`
  - strings: 'WngFB\\'

### 0x07D4  (2004)   2 packets

- **RECV** x2  size 36 B
  - (opaque / non-protobuf)

### 0x07D5  (2005)   1 packets

- **RECV** x1  size 18 B
  - `f1=0, f2=0, f3=0, f4=0, f6={ f1=0, f2=0 }`

### 0x07D8  (2008)   1 packets

- **RECV** x1  size 200 B
  - `f1=1353102, f2=0, f3=1031008, f4=2, f5=126630, f6=0, f7=328655532, f8=0, f9=0, f10=0, f11=0, f12=11014594, f13=36, f14=39`
  - strings: 'NOMATE_NAME@@' | "h$p'x" | 'halms'

### 0x07DF  (2015)   281 packets

- **SEND** x51  size 16 B
  - (opaque / non-protobuf)
- **RECV** x230  size 16-96 B
  - (opaque / non-protobuf)
  - strings: 'STR_ID_tZhongQiu2026Npc[37341][Text115]@@2026/09/15 01:00 - 2026/10/05 08:59@@'

### 0x07E4  (2020)   48 packets

- **SEND** x24  size 32 B
  - (opaque / non-protobuf)
- **RECV** x24  size 32 B
  - (opaque / non-protobuf)

### 0x07E7  (2023)   1 packets

- **RECV** x1  size 179 B
  - `f1={ f1=1, f2=102, f3=0 }, f1={ f1=1, f2=103, f3=0 }, f1={ f1=1, f2=104, f3=0 }, f1={ f1=1, f2=105, f3=0 }, f1={ f1=1, f2=106, f3=0 }, f1={ f1=2, f2=202, f3=0 }, f1={ f1=2, f2=205, f3=0 }, f1={ f1=2, f2=207, f3=0 }, f1={ f1=3, f2=301, f3=0 }, f1={ f1=3, f2=304, f3=0 }, f1={ f1=4, f2=403, f3=0 }, f1={ f1=4, f2=404, f3=0 }, f1={ f1=4, f2=407, f3=0 }, f1={ f1=4, f2=408, f3=0 }`

### 0x07E9  (2025)   35 packets

- **RECV** x35  size 25-41 B
  - `f1=3999900641, f2={ f1=2, f2=78, f3=172, f3=4, f3=200, f3=0 }, f2={ f1=2, f2=120, f3=342, f3=0, f3=0, f3=0 }`

### 0x07EB  (2027)   2 packets

- **SEND** x1  size 12 B
  - `f1=3, f2=24702, f3=0`
- **RECV** x1  size 12 B
  - `f1=3, f2=24702, f3=13`

### 0x07F1  (2033)   1 packets

- **RECV** x1  size 44 B
  - `f12=0x0000000000000000`

### 0x07FB  (2043)   1 packets

- **RECV** x1  size 20 B
  - `f1={ f1=1, f2=1, f3=2 }, f1={ f1=6, f2=0, f3=2 }`

### 0x07FE  (2046)   833 packets

- **RECV** x833  size 14-83 B
  - `f1=3999900669, f2={ f1=25, f3=1073741824, f3=2, f3=0, f3=0, f3=0, f3=0, f3=0, +4 }, f2={ f1=120, f4=413, f5=10, f6=3000, f7=0, f8=0, f9=0 }`

### 0x0809  (2057)   14 packets

- **RECV** x14  size 23-25 B
  - `f1=132465, f2=12080, f3=3, f4=0, f5=0, f6=1, f7=0, f8=0, f9=0`

### 0x080E  (2062)   8 packets

- **RECV** x8  size 277-988 B
  - `f1=4, f2={ f1=4100016262, f2=3999900685, f3=0, f4=123309, f5=4137, f6=4198, f7=0, f8=1, +4 }`

### 0x0810  (2064)   1 packets

- **RECV** x1  size 8 B
  - `f1=0, f2=0`

### 0x0817  (2071)   2 packets

- **SEND** x2  size 6 B
  - `f1=1`

### 0x081D  (2077)   1 packets

- **RECV** x1  size 134 B
  - `f1=0, f5={ f1=0, f2=2, f3=1, f4=0, f5=0, f5=1 }, f5={ f1=1, f2=103, f3=1, f4=0 }, f5={ f1=2, f2=1, f3=1, f4=0 }, f5={ f1=3, f2=1, f3=1, f4=0 }, f5={ f1=4, f2=1, f3=1, f4=0 }, f5={ f1=5, f2=1, f3=1, f4=0 }, f5={ f1=6, f2=1, f3=1, f4=0 }, f5={ f1=7, f2=1, f3=1, f4=0 }, f5={ f1=8, f2=1, f3=1, f4=0 }, f5={ f1=9, f2=1, f3=1, f4=0 }, f5={ f1=11, f2=0, f3=1, f4=0 }, f5={ f1=12, f2=1, f3=1, f4=0 }, f6=1353102`

### 0x081E  (2078)   5 packets

- **SEND** x2  size 10 B
  - `f1=3999900685`
- **RECV** x3  size 8-10 B
  - `f1=3999900685`

### 0x0822  (2082)   1 packets

- **RECV** x1  size 27 B
  - `f1={ f1=186, f2=1353102, f8=0, f9=0, f10=0, f11=0, f17=0, f18=0 }`

### 0x082A  (2090)   1 packets

- **RECV** x1  size 12 B
  - `f1=0, f2=0, f3=0, f4=0`

### 0x082B  (2091)   4 packets

- **RECV** x4  size 350-1020 B
  - `f1={ f1=186, f2=1, f3="DuneWanderer", f4=3935, f5=328, f6=198, f7=0, f8=9, +2 }, f1={ f1=2, f2=0, f3="Emerald", f7=0, f8=1, f9=1, f10=1 }, f1={ f1=4, f2=0, f3="Honor", f7=0, f8=1, f9=7, f10=1 }, f1={ f1=5, f2=0, f3="Hebby", f7=0, f8=1, f9=16, f10=1 }, f1={ f1=6, f2=0, f3="Volcano", f7=0, f8=1, f9=12, f10=13 }, f1={ f1=7, f2=0, f3="Triumph", f7=0, f8=1, f9=9, f10=13 }, f1={ f1=8, f2=0, f3="Faith", f7=0, f8=1, f9=1, f10=13 }, f1={ f1=11, f2=0, f3="Eternity", f7=0, f8=1, f9=5, f10=13 }, f1={ f1=12, f2=0, f3="Lion", f7=0, f8=1, f9=10, f10=13 }, f1={ f1=13, f2=0, f3="Lucky7", f7=0, f8=1, f9=13, f10=13 }`
  - strings: 'DuneWanderer ' | 'Emerald8' | 'Eternity8' | 'Faith8' (+7 more)

### 0x082D  (2093)   2 packets

- **RECV** x2  size 22 B
  - (opaque / non-protobuf)

### 0x0833  (2099)   1386 packets

- **SEND** x255  size 9-124 B
  - `f12=162, f24="</F>PlotEditorLuaFuncCallback</S>PlotEditorOnInstancePath..."`
  - strings: 'r</F>PlotEditorLuaFuncCallback</S>PlotEditorOnInstancePathFindCompleted</N>9942</N>19</N>1353102</N>37327</N>1</N>1'
- **RECV** x1131  size 14-568 B
  - `f1=1353102, f3=0, f9=140894171, f12=258, f13=0, f14=0, f15=0, f19=0, f22=0`
  - strings: "InfiniteReceiverData_Ex|10511000|1|1|{['Data']={['ServerType']=2,['Info']={{['Flag']=0,['Id']='10976',['Red']=0,['IsSelect']=1},{['Flag']=0,['Id']='10978',['Red']=0,['IsSelect']=0},{['Flag']=0,['Id']='10719',['Red']=0,['Is"

### 0x0838  (2104)   439 packets

- **SEND** x62  size 31-53 B
  - `f1=12768761, f4=3999900739, f5=2381060723, f7=39776, f8=43400, f13=24, f16=18446744071795645043, f19=30288, f20=52770`
- **RECV** x377  size 32-57 B
  - `f3=0, f4=3999900739, f5=3999900669, f7=243, f8=179, f13=2, f15=1, f21=18446744073709551484, f24=0, f25=34359738369, f26=0, f27=0`

### 0x083B  (2107)   1 packets

- **RECV** x1  size 77 B
  - `f1={ f1=285, f2=84, f3=0 }, f1={ f1=685, f2=44, f3=0 }, f1={ f1=700, f2=0, f3=0 }, f1={ f1=820, f2=81, f3=0 }, f1={ f1=894, f2=37, f3=0 }, f1={ f1=894, f2=38, f3=0 }, f1={ f1=894, f2=46, f3=0 }, f1={ f1=70001, f2=1, f3=0 }`

### 0x0842  (2114)   5 packets

- **SEND** x3  size 22-30 B
  - (opaque / non-protobuf)
- **RECV** x2  size 14-22 B
  - (opaque / non-protobuf)

### 0x0846  (2118)   1 packets

- **RECV** x1  size 132 B
  - (opaque / non-protobuf)
  - strings: 'halms'

### 0x0849  (2121)   3 packets

- **SEND** x1  size 6 B
  - `f1=0`
- **RECV** x2  size 150 B
  - `f1=2, f2=1, f3={ f1=659228, f2=0, f3=0, f4=604800, f5=13227953, f6=0, f7=0, f8=0, +4 }`
  - strings: 'J2STR_ID_tZhongQiu2026Dragon_DelText[Mail][Sender]@@R1STR_ID_tZhongQiu2026Dragon_DelText[Mail][Title]@@X'

### 0x084A  (2122)   5 packets

- **RECV** x5  size 12-14 B
  - `f1=0, f2=3999900739, f3=0`

### 0x084E  (2126)   14 packets

- **SEND** x1  size 36 B
  - (opaque / non-protobuf)
- **RECV** x13  size 36-888 B
  - (opaque / non-protobuf)

### 0x084F  (2127)   3 packets

- **RECV** x3  size 20 B
  - `f28=0x00000f000000bf44`

### 0x0855  (2133)   1 packets

- **RECV** x1  size 22 B
  - (opaque / non-protobuf)

### 0x0858  (2136)   4 packets

- **RECV** x4  size 32-336 B
  - (opaque / non-protobuf)

### 0x085C  (2140)   2 packets

- **RECV** x2  size 6 B
  - (opaque / non-protobuf)

### 0x085D  (2141)   1 packets

- **RECV** x1  size 12 B
  - (opaque / non-protobuf)

### 0x0861  (2145)   1 packets

- **RECV** x1  size 44 B
  - `f1=0, f2=1353102, f3=0, f4=1790813828, f5=10, f6=1790821028, f7=1790813829, f8=0, f9=0, f10=1, f11=0, f13=0, f14=0`

### 0x0867  (2151)   4 packets

- **SEND** x2  size 8-9 B
  - `f1=0, f2=271`
- **RECV** x2  size 8-9 B
  - `f1=0, f2=271`

### 0x0869  (2153)   1 packets

- **RECV** x1  size 32 B
  - (opaque / non-protobuf)

### 0x086D  (2157)   14 packets

- **RECV** x14  size 80 B
  - (opaque / non-protobuf)

### 0x086F  (2159)   1 packets

- **RECV** x1  size 6 B
  - (opaque / non-protobuf)

### 0x0875  (2165)   1 packets

- **RECV** x1  size 21 B
  - `f1=0, f2=1353102, f3=0, f4=0, f5=1, f6=186, f7=<empty>`

### 0x088A  (2186)   1 packets

- **RECV** x1  size 6 B
  - `f1=16`

### 0x0896  (2198)   26 packets

- **RECV** x26  size 17-41 B
  - (opaque / non-protobuf)

### 0x0898  (2200)   963 packets

- **SEND** x333  size 20-23 B
  - `f1=153, f2=3999900739, f3=1, f4=12496162, f5=11391`
- **RECV** x630  size 17-20 B
  - `f1=158, f2=3999900641, f3=1, f4=140894148`

### 0x089A  (2202)   8 packets

- **RECV** x8  size 32 B
  - (opaque / non-protobuf)

### 0x089B  (2203)   1 packets

- **RECV** x1  size 8 B
  - (opaque / non-protobuf)

### 0x089D  (2205)   1 packets

- **SEND** x1  size 188 B
  - (opaque / non-protobuf)
  - strings: '502E91C8CA8E'

### 0x08A3  (2211)   2 packets

- **RECV** x2  size 104 B
  - (opaque / non-protobuf)

### 0x08B2  (2226)   11 packets

- **SEND** x8  size 14 B
  - `f1=7, f2=37341, f3=0, f4=0`
- **RECV** x3  size 14 B
  - `f1=2, f2=200038, f3=0, f4=0`

### 0x08B3  (2227)   4 packets

- **RECV** x4  size 29 B
  - `f1=0, f2=126, f3=8, f4=272, f5=30, f6=16, f7=17, f8=8, f9=1790813828, f10=3`

### 0x08B7  (2231)   2 packets

- **RECV** x2  size 14 B
  - `f1=12, f11={ f1=1, f2=0, f3=0 }`

### 0x08B9  (2233)   1 packets

- **RECV** x1  size 8 B
  - `f1=0, f4=0`

### 0x08BA  (2234)   2 packets

- **SEND** x1  size 13 B
  - `f1=0, f2=186, f3=1325008`
- **RECV** x1  size 19 B
  - `f1=0, f2=0, f3=1353102, f4=4, f5=2290000`

### 0x08BB  (2235)   6 packets

- **SEND** x3  size 12 B
  - `f1=1, f2=3999900685`
- **RECV** x3  size 14 B
  - `f1=1, f2=3999900685, f3=0`

### 0x08C2  (2242)   1 packets

- **RECV** x1  size 284 B
  - (opaque / non-protobuf)

### 0x08CA  (2250)   1 packets

- **RECV** x1  size 84 B
  - (opaque / non-protobuf)

### 0x08CE  (2254)   1 packets

- **RECV** x1  size 136 B
  - (opaque / non-protobuf)
  - strings: 'halms'

### 0x08D1  (2257)   2 packets

- **RECV** x2  size 192 B
  - (opaque / non-protobuf)
  - strings: '1353102 0 0 -1'

### 0x08D8  (2264)   23 packets

- **RECV** x23  size 16 B
  - (opaque / non-protobuf)

### 0x08DA  (2266)   3 packets

- **RECV** x3  size 232 B
  - (opaque / non-protobuf)

### 0x08DB  (2267)   3 packets

- **RECV** x3  size 97 B
  - (opaque / non-protobuf)

### 0x08DE  (2270)   9 packets

- **SEND** x4  size 172 B
  - `f1=0xa469ee00`
- **RECV** x5  size 172 B
  - `f1=0xa469ee46, f8=0`

### 0x08E0  (2272)   116 packets

- **SEND** x3  size 12-14 B
  - `f1=3999900685, f5=16, f6=<empty>`
- **RECV** x113  size 8-27 B
  - `f2=1353102, f5=24, f6="DoubleDance-999"`
  - strings: 'DoubleDance-999'

### 0x08E3  (2275)   1 packets

- **RECV** x1  size 4 B
  - (opaque / non-protobuf)

### 0x08EC  (2284)   2 packets

- **RECV** x2  size 71 B
  - `f1=0, f2=3999900685, f3=65535, f4=0, f5=1, f6=1, f7=1, f8=1, f9=1, f10=0, f11=0, f12=0, f13=0, f14=0`

### 0x08ED  (2285)   1 packets

- **RECV** x1  size 72 B
  - (opaque / non-protobuf)

### 0x08F2  (2290)   1 packets

- **RECV** x1  size 12 B
  - (opaque / non-protobuf)

### 0x08F4  (2292)   378 packets

- **RECV** x378  size 55-358 B
  - `f1=3999900637, f2=3262007, f3=247, f4=242, f5=4, f7=250, f9=334, f10=<22B 000200000000000000000000>, f11=1043, f12=4, f14=107, f15=1, f16=65, f17=114089`

### 0x08F6  (2294)   1 packets

- **SEND** x1  size 520 B
  - (opaque / non-protobuf)

### 0x08F7  (2295)   1 packets

- **RECV** x1  size 268 B
  - (opaque / non-protobuf)
  - strings: 'HuntreX' | 'karma'

### 0x08FB  (2299)   2 packets

- **RECV** x2  size 126 B
  - (opaque / non-protobuf)

### 0x0902  (2306)   1 packets

- **RECV** x1  size 16 B
  - (opaque / non-protobuf)

### 0x090B  (2315)   1 packets

- **RECV** x1  size 520 B
  - (opaque / non-protobuf)
  - strings: 'STR_ID_tZhongQiu2026Dragon_DelText[Mail][Content]@@'

### 0x090C  (2316)   2 packets

- **SEND** x1  size 62 B
  - `f1={ f7=0 }`
- **RECV** x1  size 70 B
  - `f1={ f11=0x04431072, f139807=0x2b29c5f0, f1378=0xba4c8776 }`

### 0x090D  (2317)   2 packets

- **RECV** x2  size 17 B
  - `f1=4, f2=45, f3=0, f4=20260930, f5=3`

### 0x0918  (2328)   2 packets

- **RECV** x2  size 47-331 B
  - `f1=3, f2={ f1=1, f3=0, f4=0 }, f2={ f1=2, f3=0, f4=0 }, f2={ f1=3, f3=0, f4=0 }, f2={ f1=4, f3=0, f4=0 }, f2={ f1=5, f3=0, f4=0 }, f2={ f1=6, f3=0, f4=0 }, f2={ f1=7, f3=0, f4=0 }, f2={ f1=8, f3=0, f4=0 }, f2={ f1=9, f3=0, f4=0 }, f2={ f1=10, f3=0, f4=0 }, f2={ f1=11, f3=0, f4=0 }, f2={ f1=12, f3=0, f4=0 }, f2={ f1=13, f3=20180408, f4=20180507 }`

### 0x091E  (2334)   1 packets

- **RECV** x1  size 10 B
  - `f1=14, f2=0, f3=0`

### 0x091F  (2335)   7 packets

- **SEND** x1  size 10 B
  - `f1=2, f2=0, f4=0`
- **RECV** x6  size 18-65 B
  - `f1=2, f2=140000004, f3=0, f4=0, f5=1, f6=1, f7=10, f8={ f1=16, f2=1630, f3=1353102, f4=1353102, f5="halms", f6="halms", f7=122, f8=5006, +4 }`
  - strings: 'halms2' | 'halms8z@'

### 0x0923  (2339)   87 packets

- **SEND** x21  size 40-136 B
  - (opaque / non-protobuf)
- **RECV** x66  size 40-136 B
  - (opaque / non-protobuf)

### 0x0925  (2341)   11 packets

- **RECV** x11  size 22-73 B
  - `f1=970003, f2=13393, f3="1,2,3,5,9,13,15,16,17,18,19,20,24,30,34,35,40,51,53,44", f4=6058, f5=13393`
  - strings: '61,2,3,5,9,13,15,16,17,18,19,20,24,30,34,35,40,51,53,44 '

### 0x092D  (2349)   1 packets

- **RECV** x1  size 64 B
  - `f1=0, f2=0, f3={ f1=3326046, f2=0 }, f3={ f1=3336737, f2=0 }, f3={ f1=3341901, f2=0 }, f3={ f1=3342836, f2=0 }, f3={ f1=3346027, f2=0 }, f3={ f1=3349735, f2=0 }, f6=0`

### 0x092E  (2350)   147 packets

- **RECV** x147  size 120-136 B
  - `f1=12244252, f2=4050001, f3=1, f4=1, f5=1, f6=0, f7=0, f8=0, f9=0, f10=0, f11=0, f12=0, f13=0, f14=0`

### 0x0935  (2357)   1 packets

- **RECV** x1  size 8 B
  - `f1=0, f2=54`

### 0x0939  (2361)   577 packets

- **RECV** x577  size 55-299 B
  - `f1=4294967295, f2=2021, f3=0, f4=1621, f6=103, f7=0, f8=1, f9=0, f10=0, f13=1411872, f14="MinaPiano11", f14="All ", f14=<empty>`
  - strings: ' : minaakapiano -' | 'All r' | 'MinaPiano11r'

### 0x093A  (2362)   11 packets

- **RECV** x11  size 13-20 B
  - `f1=18446744073414485061, f2=442, f3=0`

### 0x093C  (2364)   1 packets

- **SEND** x1  size 6 B
  - `f1=1`

### 0x093E  (2366)   2 packets

- **RECV** x2  size 516 B
  - (opaque / non-protobuf)

### 0x093F  (2367)   7 packets

- **SEND** x1  size 12 B
  - `f1=1, f2=3999900685`
- **RECV** x6  size 14-98 B
  - `f1=0, f2=1353102, f3=122, f4={ f1=2, f2=57600 }, f4={ f1=5, f2=1830 }, f4={ f1=7, f2=2565 }, f4={ f1=8, f2=2000 }, f4={ f1=9, f2=4350 }, f4={ f1=10, f2=4000 }, f4={ f1=11, f2=3200 }, f4={ f1=12, f2=27690 }, f4={ f1=15, f2=700 }, f4={ f1=16, f2=200 }, f4={ f1=20, f2=3572 }`

### 0x0942  (2370)   6 packets

- **SEND** x6  size 111-901 B
  - (opaque / non-protobuf)

### 0x0952  (2386)   1 packets

- **RECV** x1  size 28 B
  - (opaque / non-protobuf)

### 0x0953  (2387)   2 packets

- **RECV** x2  size 12 B
  - (opaque / non-protobuf)

### 0x0956  (2390)   2 packets

- **RECV** x2  size 18 B
  - `f1=3999900669, f2=3999900739, f3=6`

### 0x095E  (2398)   1 packets

- **RECV** x1  size 267 B
  - `f1=2, f2=3999900685, f3={ f1=410, f2=0, f3=64, f4=2, f5=0, f6=0, f7=0, f8=0, +4 }, f3={ f1=420, f2=0, f3=256, f4=1, f5=10, f6=0, f7=0, f8=0, +4 }, f3={ f1=430, f2=0, f3=0, f4=1, f5=0, f6=0, f7=0, f8=0, +4 }, f3={ f1=440, f2=0, f3=128, f4=1, f5=0, f6=0, f7=0, f8=0, +4 }, f3={ f1=450, f2=0, f3=512, f4=1, f5=0, f6=0, f7=0, f8=0, +4 }, f3={ f1=460, f2=0, f3=16, f4=1, f5=0, f6=0, f7=0, f8=0, +4 }, f3={ f1=480, f2=0, f3=0, f4=1, f5=0, f6=0, f7=0, f8=0, +4 }, f3={ f1=481, f2=0, f3=8, f4=1, f5=40, f6=0, f7=0, f8=0, +4 }`

### 0x0963  (2403)   62 packets

- **RECV** x62  size 24-30 B
  - `f1=102322, f2=19664692, f3=0, f4=213, f5=195, f6=30190, f7=2, f8=128`

### 0x0973  (2419)   2 packets

- **SEND** x1  size 14 B
  - `f1=4, f2=3999900685, f4=0`
- **RECV** x1  size 173 B
  - `f1=4, f2=3999900685, f3={ f1=201, f2=3, f3=1680, f4=0, f5=0, f6=0, f7=0, f8=0, +4 }, f3={ f1=301, f2=1, f3=400, f4=0, f5=0, f6=0, f7=0, f8=0, +4 }, f3={ f1=401, f2=5, f3=1680, f4=0, f5=0, f6=0, f7=0, f8=0, +4 }, f3={ f1=1301, f2=5, f3=1000, f4=0, f5=0, f6=0, f7=0, f8=0, +4 }, f3={ f1=1302, f2=1, f3=0, f4=0, f5=0, f6=0, f7=0, f8=0, +4 }, f4=0`

### 0x0975  (2421)   2 packets

- **SEND** x1  size 22 B
  - (opaque / non-protobuf)
- **RECV** x1  size 18 B
  - (opaque / non-protobuf)

### 0x0976  (2422)   209 packets

- **RECV** x209  size 21-119 B
  - `f1=1436897, f3=435, f4=285, f5=7030, f6=8, f8=0, f9=0, f10={ f1=414271, f2=31900, f3=0, f4=0, f5=0, f6=0, f7=0, f8=0, +1 }, f10={ f1=420229, f2=31900, f3=0, f4=0, f5=0, f6=0, f7=0, f8=0, +1 }, f10={ f1=416769, f2=31900, f3=0, f4=0, f5=0, f6=0, f7=0, f8=0, +1 }, f10={ f1=414370, f2=31900, f3=0, f4=0, f5=0, f6=0, f7=0, f8=0, +1 }`

### 0x097B  (2427)   13 packets

- **SEND** x5  size 16-20 B
  - `f1=4100017592, f3=0, f6=4, f7=4004200796`
- **RECV** x8  size 14-86 B
  - `f3=1, f6=46, f8=0, f11=4100015743, f12=4100015992, f13=4100015993, f14=4100016035, f15=4100015995, f16=4100015996, f17=0, f18=4100015997, f19=0, f20=0, f21=0`

### 0x097D  (2429)   2 packets

- **SEND** x2  size 52 B
  - (opaque / non-protobuf)
  - strings: 'V&Y=4$'

### 0x0983  (2435)   4 packets

- **SEND** x2  size 20 B
  - (opaque / non-protobuf)
- **RECV** x2  size 20-84 B
  - (opaque / non-protobuf)

### 0x0988  (2440)   651 packets

- **SEND** x23  size 25 B
  - `f1=10560717, f2=2031394970, f4=448, f5=270, f8=3, f13=0`
- **RECV** x628  size 20-121 B
  - `f1=900031, f2=5471, f4=176, f5=221, f7=0, f8=11, f11=0, f14=1, f16=0, f17=0, f18=0, f19=0, f21=0, f23=0`
  - strings: 'STR_TRAP_ID_5471@@'

### 0x0990  (2448)   1 packets

- **RECV** x1  size 102 B
  - `f1=1, f2=3999900685, f3={ f1=400, f2=3, f3=1, f4=0, f5=0 }, f3={ f1=402, f2=3, f3=2, f4=0, f5=0 }, f3={ f1=304, f2=3, f3=3, f4=0, f5=0 }, f3={ f1=303, f2=3, f3=4, f4=0, f5=0 }, f3={ f1=505, f2=2, f3=5, f4=0, f5=0 }, f3={ f1=503, f2=2, f3=6, f4=0, f5=0 }, f3={ f1=102, f2=1, f3=7, f4=0, f5=0 }`

### 0x0995  (2453)   14 packets

- **RECV** x14  size 10 B
  - `f1=2, f2=0, f3=0`

### 0x0998  (2456)   1 packets

- **RECV** x1  size 12 B
  - `f1=0, f2=0, f3=0, f4=0`

### 0x099D  (2461)   1 packets

- **RECV** x1  size 19 B
  - (opaque / non-protobuf)

### 0x09AF  (2479)   1 packets

- **RECV** x1  size 6 B
  - `f1=9`

### 0x09CB  (2507)   1 packets

- **RECV** x1  size 16 B
  - `f1=1, f4=0, f5=0, f6=0, f6=0, f6=0`

### 0x09E4  (2532)   5 packets

- **RECV** x5  size 6-10 B
  - `f1=60, f10=0, f12=0`

### 0x09EE  (2542)   2 packets

- **RECV** x2  size 16-18 B
  - `f1=1, f2=0, f3=0, f5=3999900739, f6=1`

### 0x09F0  (2544)   1 packets

- **RECV** x1  size 10 B
  - `f1=0, f2=0, f3=0`

### 0x09F9  (2553)   3 packets

- **SEND** x3  size 10 B
  - `f1=4, f9=1, f10=4`

### 0x0A2A  (2602)   1 packets

- **RECV** x1  size 6 B
  - `f1=0`

### 0x0A2B  (2603)   2 packets

- **RECV** x2  size 18-20 B
  - `f1=5, f2=0, f3=0, f5=3999900685, f6=0, f7=0`

### 0x0A2C  (2604)   5 packets

- **SEND** x5  size 10-12 B
  - `f1=2, f2=3999900685`

### 0x0A36  (2614)   272 packets

- **SEND** x136  size 12 B
  - (opaque / non-protobuf)
- **RECV** x136  size 12 B
  - (opaque / non-protobuf)

### 0x0A39  (2617)   1 packets

- **RECV** x1  size 16 B
  - `f1={ f1=3, f2=0 }, f1={ f1=4, f2=0 }`

### 0x2338  (9016)   1 packets

- **RECV** x1  size 24 B
  - `f1=0, f2=1, f2=0, f2=0, f2=0, f2=0, f2=30, f2=4294967295`

### 0x233B  (9019)   1 packets

- **RECV** x1  size 10 B
  - `f1=2, f2=0, f3=0`

### 0x234C  (9036)   3 packets

- **RECV** x3  size 584-1022 B
  - `f1=1, f4={ f1=23, f2=1, f3=20260930161708, f4=0, f7=0 }, f4={ f1=1, f2=48, f3=0, f4=0, f5=459712 }, f4={ f1=1, f2=50, f3=0, f4=0, f5=459712 }, f4={ f1=1, f2=51, f3=0, f4=0, f5=459712 }, f4={ f1=27, f2=1, f3=0, f4=0, f7=0 }, f4={ f1=16, f2=0, f3=0, f4=3, f7=0 }, f4={ f1=21, f2=0, f3=0, f4=3, f7=0 }, f4={ f1=19, f2=0, f3=0, f4=0, f7=0 }, f4={ f1=20, f2=1, f3=0, f4=0, f7=0 }, f4={ f1=24, f2=100, f3=0, f4=0, f7=0 }, f4={ f1=24, f2=101, f3=0, f4=0, f7=0 }, f4={ f1=24, f2=102, f3=0, f4=0, f7=0 }, f4={ f1=24, f2=103, f3=0, f4=0, f7=0 }`

### 0x2379  (9081)   1 packets

- **RECV** x1  size 22 B
  - `f1=1, f2=0, f3=0, f4=999, f5=1000000000, f17=0`

### 0x2393  (9107)   2 packets

- **RECV** x2  size 13-17 B
  - `f1=9, f16={ f1=1, f2=0, f3=0, f4=0 }`

### 0x23AA  (9130)   1 packets

- **RECV** x1  size 24 B
  - `f1=0, f2=3, f3=0, f4=0, f5={ f1=6, f2=0, f3=0, f4=0 }, f11=1`

### 0x23C4  (9156)   1 packets

- **RECV** x1  size 17 B
  - `f1=8, f2=0, f3=0, f8=0, f9=0, f19=0`

### 0x23DD  (9181)   1 packets

- **RECV** x1  size 19 B
  - `f1=0, f2=0, f3=0, f4=0, f5=20260918, f6=0`

### 0x3AF6  (15094)   1 packets

- **RECV** x1  size 98 B
  - `f5=0, f6=0x78ab8e1cfb6cc8ab`

### 0x3C61  (15457)   1 packets

- **RECV** x1  size 28 B
  - `f13=0x008ac925`

### 0x3CE8  (15592)   1 packets

- **RECV** x1  size 26 B
  - `f9=0x00ca50f6`

### 0x3F34  (16180)   1 packets

- **RECV** x1  size 58 B
  - `f10=0x00649432`

### 0x3F75  (16245)   1 packets

- **RECV** x1  size 12 B
  - (opaque / non-protobuf)

### 0x4274  (17012)   1 packets

- **RECV** x1  size 58 B
  - (opaque / non-protobuf)
  - strings: 'l|(21'

### 0x4310  (17168)   1 packets

- **RECV** x1  size 26 B
  - (opaque / non-protobuf)

### 0x4422  (17442)   1 packets

- **RECV** x1  size 34 B
  - `f6=0x000eb3dd`

### 0x47AD  (18349)   1 packets

- **RECV** x1  size 108 B
  - (opaque / non-protobuf)

### 0x486D  (18541)   1 packets

- **RECV** x1  size 108 B
  - (opaque / non-protobuf)
  - strings: 'y^Eagw_'

### 0x48E4  (18660)   1 packets

- **RECV** x1  size 58 B
  - `f9={ f181539=4840, f939=78 }`
  - strings: '&\\FB;'

### 0x49FE  (18942)   1 packets

- **RECV** x1  size 34 B
  - `f14=0x0a2b870e`

### 0x4A61  (19041)   1 packets

- **RECV** x1  size 28 B
  - (opaque / non-protobuf)

### 0x4ABD  (19133)   1 packets

- **RECV** x1  size 44 B
  - `f3=100`

### 0x4CE8  (19688)   1 packets

- **RECV** x1  size 26 B
  - `f7=0x00dd72ab`

### 0x4CED  (19693)   1 packets

- **RECV** x1  size 108 B
  - (opaque / non-protobuf)

