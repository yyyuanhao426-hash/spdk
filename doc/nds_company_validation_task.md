# NDS 涓婃満楠岃瘉浠诲姟涔︼紙澶栭儴 鈫愨啋 鍐呴儴 AI 瀵硅瘽鏂囦欢锛?

> 浣犲ソ锛屽唴閮?AI銆傛湰鏂囦欢鏄?*浠诲姟涔?+ 瀵硅瘽鏂囦欢**锛屽瓨鏀惧湪 spdk-urma 浠撳簱
> `nds_v1` 鍒嗘敮鐨?`doc/` 鐩綍涓嬨€?

## 瀵硅瘽鍗忚锛堝厛璇昏繖涓級

1. **璇诲彇浠诲姟**锛歚git pull origin nds_v1` 鍚庤鏈枃浠讹紝鎸夌 0~6 鑺傛墽琛屻€?
2. **鍥炲啓缁撴灉**锛氭妸鎵ц杩涘睍銆佸叏閮ㄥ懡浠よ緭鍑哄拰鏈€缁堟姤鍛?*杩藉姞鍒版湰鏂囦欢鏈熬鐨?
   銆岀 9 鑺?鍐呴儴 AI 鍥炴墽鍖恒€?*锛堟寜鏃ユ湡鍒嗘潯鐩紝鎶ュ憡鍏ㄦ枃璐村叆锛屼笉瑕佸彟寤烘枃浠躲€?
   涓嶈鎬荤粨鏀瑰啓銆佷笉瑕佸垹闄?淇敼鏈枃浠跺凡鏈夊唴瀹癸級銆?
3. **鍥炰紶**锛歚git add doc/nds_company_validation_task.md &&
   git commit -m "docs(nds-task): <鏈鍋氫簡浠€涔?" && git push origin nds_v1`銆?
4. **寰€杩?*锛氬閮?AI 浼?pull 璇诲彇浣犵殑鍥炴墽锛屾妸涓嬩竴姝ユ寚浠よ拷鍔犲埌銆岀 10 鑺?
   澶栭儴 AI 鎸囦护鍖恒€嶅啀 push锛涗綘闅忔椂 pull 鏌ョ湅鏂版寚浠ゃ€?
5. 鍗′綇/寮傚父鏃讹細涓嶈鑷鍙戞尌淇锛屽師鏍疯褰曠幇璞″悗鎸夌 2 姝ュ洖浼犲苟娉ㄦ槑
   "绛夊緟澶栭儴鎸囦护"銆?

- **鍘熷垯**锛氭墍鏈夊懡浠よ緭鍑哄繀椤诲師鏍蜂繚鐣欙紙鎴愬姛鍜屾姤閿欓兘瑕侊級锛涗笉瑕佷慨鏀逛换浣曚唬鐮侊紱
  涓嶈鎵ц鏈枃妗ｅ垪鍑轰箣澶栫殑鍐欐搷浣?鍒犻櫎鎿嶄綔銆?

## 0. 鑳屾櫙涓€鍙ヨ瘽

鎴戜滑鍦ㄥ仛 NDS锛圢PU HBM 缁?URMA 鐩磋繛杩滅 NVMe锛屼豢鐓у凡鏈?GPU 鐗?GDS锛夈€?
澶栭儴寮€鍙戝凡瀹屾垚 SPDK 灞備唬鐮侊紙鍒嗘敮 `nds_v1`锛夛紝鏈涓婃満鐩爣锛?
**鈶?纭鐜锛涒憽 缂栬瘧閫氳繃锛涒憿 璺戦€氭祴璇曪紱鈶?閲囬泦涓変釜纭欢楠岃瘉椤圭殑鏁版嵁銆?*

## 0.5 鍏变韩鑺傜偣闅旂瀹堝垯锛?97 涓哄浜哄叡鐢紝蹇呴』閬靛畧锛?

197 鑺傜偣鏈夊緢澶氱敤鎴峰叡鐢紝鎵€鏈夋搷浣滀互"涓嶆墦鎵颁粬浜?涓哄墠鎻愶細

1. **浜х墿闅旂**锛氫唬鐮併€佺紪璇戜骇鐗┿€佷复鏃舵枃浠跺叏閮ㄦ斁鑷繁 home 鐩綍
   锛堝 /home/l00955908/nds/锛夛紝绂佹鍐欏叆 /home/xxx銆?home/yin 绛変粬浜虹洰褰?
   锛堝彲璇讳笉鍙啓锛夛紝绂佹鍐?/tmp 涔嬪鐨勭郴缁熺洰褰曘€?
2. **鎼滅储闄愬煙**锛氱姝?`find /` 鍏ㄧ洏鎼滅储锛涢檺瀹氳矾寰勶紙/usr/local/Ascend銆?
   /usr/lib64銆?opt銆?HOME锛夊苟鍔犺秴鏃讹紝濡?
   `timeout 60 find <璺緞...> -name <pattern> 2>/dev/null`銆?
3. **缂栬瘧闄愭牳**锛歚make -j16`锛堜笉瑕?-j$(nproc)锛夛紝鍙姞 nice 闄嶄紭鍏堢骇锛?
   `nice -n 10 make -j16`锛涚紪璇戝敖閲忛敊宄帮紙閬垮紑鏁存満楂樿礋杞芥椂娈碉級銆?
4. **SPDK 澶ч〉鍐呭瓨**锛歶rma_perf 鍒濆鍖栦細娑堣€?hugepages銆傝窇鍓嶅厛璁板綍鐜扮姸
   锛坄grep -i huge /proc/meminfo`锛夛紝鍙仛鐭椂娴嬭瘯锛?t 5锛夛紝娴嬪畬纭澶ч〉
   閲婃斁锛涜嫢闇€瑕佽皟澶?hugepages锛屾祴鍚庡繀椤绘仮澶嶅師鍊煎苟璁板綍銆?
5. **缁戞牳闄愬畾**锛歶rma_perf 鐢?`-m` 鏄惧紡鎸囧畾灏戦噺鏍革紙濡?`-m 0x3` 鍙敤澶翠袱涓牳锛夛紝
   涓嶈璁?SPDK 鍗犳弧鎵€鏈夋牳銆?
6. **URMA 璁惧鍏变韩**锛歎RMA 缃戝崱/璁惧鏄暣鏈哄叡浜殑锛岃窇娴嬭瘯鍓嶇‘璁ゆ棤浠栦汉鍦ㄧ敤
   锛堝闂鐞嗗憳/鐪嬫槸鍚︽湁鍏朵粬 URMA 杩涚▼锛夛紝娴嬭瘯绐楀彛灏介噺鐭€?
7. **涓嶆敼绯荤粺閰嶇疆**锛氫笉瀹夎/鍗歌浇绯荤粺鍖呫€佷笉鏀瑰唴鏍稿弬鏁帮紙闄ら潪绗?4 鏉＄殑
   hugepages 涓旈』鎭㈠锛夛紱**涓ョ**鍦ㄥ叡浜満涓?insmod/rmmod 浠讳綍鍐呮牳妯″潡
   锛堝寘鎷皢鏉?Phase 2 鐨?.ko锛屽眾鏃跺崟鐙崗璋冪淮鎶ょ獥鍙ｏ級銆?
8. **鐮村潖鎬у啓淇濇姢**锛歶rma_perf 浼氳鍐?target 鐩樻暟鎹€斺€斿彧鍏佽瀵瑰凡纭鐨?
   绌洪棽鐩?涓撶敤娴嬭瘯鐩樿繘琛岋紝涓旇窇鍓嶈褰曘€佽窇鍚庢敞鏄庛€?

杩濆弽浠ヤ笂浠讳竴鏉″鑷村奖鍝嶄粬浜烘椂锛岀珛鍗冲仠姝㈡搷浣溿€佸師鏍疯褰曘€佸洖浼犵瓑寰呮寚浠ゃ€?

## 1. 鏈哄櫒瑙掕壊璇嗗埆锛堜袱鍙伴兘瑕佽窇锛?

```bash
hostname; ip a | grep -E "inet " | head -5
npu-smi info
nvme list
```

鍒ゅ畾瑙勫垯锛?
- `npu-smi info` 鏈夋甯歌〃鏍艰緭鍑虹殑鏈哄櫒 = **鑺傜偣 1锛圛nitiator锛孨PU 鏈猴級**
- `npu-smi` 涓嶅瓨鍦ㄤ絾 `nvme list` 鏈夌洏鐨勬満鍣?= **鑺傜偣 2锛圱arget锛岀‖鐩樻満锛?*
- 鎶婁袱鍙扮殑涓绘満鍚?IP/瑙掕壊璁板叆鎶ュ憡銆?

## 2. 鐜浣撴锛堝師鏍疯褰曞叏閮ㄨ緭鍑猴級

**涓ゅ彴閮借窇锛?*

```bash
uname -r
cat /etc/os-release | head -3
lsmod | grep -E "udma|urma|ubus|ubcore"
which gcc make && gcc --version | head -1
```

**鑺傜偣 1锛圢PU 鏈猴級璺戔€斺€旂‖浠堕獙璇佷笁浠跺锛堟渶閲嶈锛夛細**

```bash
# 鈶?鍐呮牳鏄惁瀵煎嚭鏄囪吘椹卞姩鐨?pin 鐩稿叧绗﹀彿锛堝喅瀹?Phase 2 鍐呮牳妗ユ帴鎬庝箞鍐欙級
cat /proc/kallsyms | grep -i davinci | head -50
cat /proc/kallsyms | grep -iE "hmm_|davinci.*pin|pin.*davinci" | head -30

# 鈶?CANN 鐢ㄦ埛鎬佸簱浣嶇疆
find / -name "libascendcl.so" 2>/dev/null

# 鈶?CANN 鏄惁鏈?dmabuf 瀵煎嚭鑳藉姏锛堟妸 鈶?鎵惧埌鐨勮矾寰勪唬鍏ワ級
nm -D <鈶＄殑璺緞>/libascendcl.so | grep -iE "dmabuf|handle|fd|export" | head -40
```

**鑺傜偣 2锛圱arget 鏈猴級璺戔€斺€旂‘璁ゅ彲鐢ㄧ‖鐩橈細**

```bash
nvme list
lsblk
lsblk -o NAME,MOUNTPOINT,TYPE,FSTYPE,SIZE
```

鍦ㄦ姤鍛婇噷鏍囨敞鍝潡 NVMe 鐩樻槸**绌洪棽鐩?*锛堟棤 MOUNTPOINT銆佹棤 fstype銆?
闈?LVM/RAID 鎴愬憳锛夛紝浠ュ強绯荤粺鐩樼殑 BDF锛堣繖鍙扮洏缁濆涓嶈兘鍔級銆?

## 3. 鑾峰彇浠ｇ爜锛堣妭鐐?1锛?

浜岄€変竴锛?

```bash
# A. 鐩存帴 clone锛堥渶 GitHub 鍑瘉锛?
git clone -b nds_v1 https://github.com/yyyuanhao426-hash/spdk.git

# B. 濡傛灉澶栭儴宸茬粡 scp 鎷疯礉浜嗕唬鐮佺洰褰曪紝鐩存帴 cd 杩涘幓
```

楠岃瘉浠ｇ爜姝ｇ‘鎬э細

```bash
cd spdk-urma
git log -1 --oneline    # 棰勬湡鏄剧ず ddbd44a feat(nds): SPDK 灞?NPU Direct Storage 鏀寔
ls examples/nvme/urma_perf/urma_perf_npu.c   # 棰勬湡瀛樺湪
```

## 4. 瀹氫綅 UMDK 骞剁紪璇戯紙鑺傜偣 1锛?

```bash
# 鎵?UMDK锛堝悓浜婤鐜閲屽簲宸插瓨鍦紱闄愬畾璺緞锛岀姝㈠叏鐩?find锛?
timeout 60 find /usr/local /usr/lib64 /opt /home -name "urma_api.h" -not -path "*/spdk*" 2>/dev/null
```

`urma_api.h` 鎵€鍦ㄨ矾寰勫悜涓婂洖婧埌 UMDK 浠撳簱鏍圭洰褰曪紙鍏朵笅搴旀湁
`src/urma/` 绛夌洰褰曪級锛岃涓?`<UMDK>`銆傜劧鍚庯細

```bash
cd spdk-urma
./configure --with-urma=<UMDK> 2>&1 | tail -30
# 鍏变韩鏈洪檺鏍哥紪璇戯紙瀹堝垯绗?3 鏉★細绂佺敤 -j$(nproc)锛?
nice -n 10 make -j16 2>&1 | tail -50
ls -l build/examples/urma_perf
```

- 缂栬瘧閫氳繃锛氭姤鍛婇噷璁板綍 configure/make 鐨勬渶鍚?30~50 琛?+ urma_perf 鏂囦欢璺緞
- 缂栬瘧鎶ラ敊锛?*瀹屾暣澶嶅埗鎶ラ敊娈佃惤**锛堣繖鏄叧閿氦浠樼墿涔嬩竴锛夛紝涓嶈鍙粰鏈€鍚庡嚑琛?

## 5. 鏌ユ壘閮ㄧ讲鏂囨。锛堝喅瀹氳兘鍚﹁窇娴嬭瘯锛?

鍚屼簨B 鐨?GDS 娴嬭瘯鏈変竴浠介儴缃叉枃妗ｏ紝浣嗕笉鍦ㄤ唬鐮佷粨閲屻€傚湪涓ゅ彴鏈哄櫒涓婃悳绱細

```bash
find / -iname "*urma*deploy*" -o -iname "*spdk*deploy*" 2>/dev/null | head
find ~ -iname "*.md" 2>/dev/null | grep -iE "urma|spdk" | head
```

- **鎵惧埌浜?*锛堝 `spdk_urma_deploment.md`锛夛細闃呰瀹冿紝鎸夋枃妗ｅ湪鑺傜偣 2 鍚姩
  nvmf_tgt锛堝惈 NVMe 鐩樻帴绠°€乁RMA transport 閰嶇疆锛夈€傛墽琛屾帴绠″墠**蹇呴』**鍏堟寜
  鏂囨。鏍稿寰呮帴绠＄洏鏄┖闂茬洏锛涘惎鍔ㄥ悗璁板綍瀹屾暣鐨勫惎鍔ㄥ懡浠ゅ拰閰嶇疆鍐呭鍒版姤鍛娿€?
- **娌℃壘鍒?*锛氳烦杩囩 6 鑺傜殑杩愯娴嬭瘯锛屽湪鎶ュ憡閲屾敞鏄?閮ㄧ讲鏂囨。缂哄け"锛?
  鍏朵綑楠岃瘉椤圭収甯镐氦浠樸€?

## 6. 杩愯娴嬭瘯锛堣妭鐐?1锛涘墠鎻愶細绗?5 鑺傛壘鍒版枃妗ｄ笖 target 宸插氨缁級

trid 妯℃澘锛圛P/瀛愮郴缁?NQN 鎸夐儴缃叉枃妗ｆ浛鎹級锛?
`trtype:URMA adrfam:IPv4 traddr:<鑺傜偣2 IP> trsvcid:4420 subnqn:<鏂囨。涓殑NQN>`

鎸夐『搴忚窇涓夋潯锛屾瘡鏉″畬鏁磋褰曡緭鍑猴細

```bash
# 6.1 鍥炲綊锛氳€佸姛鑳界‘璁ゆ湭鐮村潖锛堜笉闇€瑕?NPU锛?
sudo ./build/examples/urma_perf -r '<trid>' -M cpu -t 5

# 6.2 NPU staged 璺嚎锛堟湰闃舵涓绘垬鍦猴紝棰勬湡閫氳繃棰勬骞跺嚭甯﹀鏁版嵁锛?
sudo ./build/examples/urma_perf -r '<trid>' -M npu-staged -t 5

# 6.3 NPU peermem 璺嚎锛堥鏈熷け璐モ€斺€斿唴鏍?NPU 妗ユ帴杩樻病鍐欙紝鏀堕泦鎶ラ敊灏辨槸鐩殑锛?
sudo ./build/examples/urma_perf -r '<trid>' -M npu -t 5
```

娉ㄦ剰浜嬮」锛?
- 鑻?6.2 鎶?`Unable to load libascendcl.so`锛岀敤绗?2 鑺?鈶?鎵惧埌鐨勫簱鐩綍璁剧疆
  鐜鍙橀噺鍚庨噸璺戯細`export LD_LIBRARY_PATH=<CANN lib64 鐩綍>:$LD_LIBRARY_PATH`
- 鑻?6.2 鎶?NPU 鍒嗛厤瀵归綈鐩稿叧閿欒锛屽師鏍疯褰曟姤閿欙紙杩欐槸椤电矑搴﹂獙璇侀」鐨勮緭鍏ワ級
- 6.3 棰勬湡鍦ㄥ唴瀛樻敞鍐岄樁娈靛け璐ワ紙濡?registration failure / -ENOTSUP / provider
  鏈敞鍐岀被鎶ラ敊锛夛紝**杩欎釜鎶ラ敊鍘熸枃鏄?Phase 2 鐨勫叧閿緭鍏?*
- 姣忔潯娴嬭瘯缁撴潫鎶?stdout+stderr 鍏ㄩ儴璁板叆鎶ュ憡

## 7. 杈撳嚭鎶ュ憡瑕佹眰

鎶ュ憡**鍏ㄦ枃**鎸変笅鏂规ā鏉垮啓鍏ャ€岀 9 鑺?鍐呴儴 AI 鍥炴墽鍖恒€嶇殑鏈€鏂版潯鐩噷锛?
**鎵€鏈?璁板綍"澶勮创鍘熷杈撳嚭锛屼笉瑕佹€荤粨鏀瑰啓**锛?

```markdown
# NDS 涓婃満楠岃瘉鎶ュ憡
鏃ユ湡锛?date>  鎵ц鑰咃細<鍐呴儴AI鏍囪瘑>

## 1. 鏈哄櫒娓呭崟
| 瑙掕壊 | 涓绘満鍚?| IP | 鍏抽敭纭欢 |
|---|---|---|---|
| Initiator(NPU) | | | NPU 鍨嬪彿/鏁伴噺锛坣pu-smi 杈撳嚭鎽樺綍锛墊
| Target | | | NVMe 鐩樺瀷鍙?BDF |

## 2. 鐜浣撴
### 2.1 涓ゅ彴鍏叡
锛坲name/os-release/lsmod/gcc 鍘熷杈撳嚭锛?
### 2.2 鑺傜偣1 纭欢楠岃瘉涓変欢濂?
- 鍐呮牳 davinci/hmm 绗﹀彿锛氾紙kallsyms grep 鍘熷杈撳嚭锛?
  - 缁撹锛氭湁 / 鏃?鍙敤浜?pin HBM 鐨勫鍑虹鍙凤紱鍒楀嚭鏈€鐩稿叧鐨勭鍙峰悕
- libascendcl.so 璺緞锛?
- nm -D 妫€鏌?dmabuf/handle/export锛氾紙鍘熷杈撳嚭锛?
  - 缁撹锛氭湁 / 鏃?dmabuf 瀵煎嚭绫荤鍙凤紱鍒楀嚭绗﹀彿鍚?
### 2.3 鑺傜偣2 纭洏
锛坣vme list / lsblk 鍘熷杈撳嚭锛涙爣娉ㄧ┖闂茬洏 BDF 涓庣郴缁熺洏 BDF锛?

## 3. 浠ｇ爜涓庣紪璇?
- git log -1 杈撳嚭锛?
- configure 灏鹃儴杈撳嚭锛?
- make 灏鹃儴杈撳嚭锛?
- 缁撹锛氱紪璇戞垚鍔?/ 澶辫触锛堝け璐ラ檮瀹屾暣鎶ラ敊锛?

## 4. 閮ㄧ讲鏂囨。
- 鏄惁鎵惧埌锛氳矾寰?/ 鏈壘鍒?
- target 鍚姩鏂瑰紡鎽樺綍锛堣嫢鎵惧埌锛夛細

## 5. 娴嬭瘯缁撴灉
### 5.1 -M cpu锛堝洖褰掞級
锛堝畬鏁磋緭鍑猴級缁撹锛氶€氳繃 / 澶辫触
### 5.2 -M npu-staged
锛堝畬鏁磋緭鍑猴紝鍚?mem_type=npu-staged 琛屻€侀妫€琛屻€佸甫瀹?寤惰繜琛岋級
缁撹锛氶€氳繃 / 澶辫触锛泂taged_copy 鑰楁椂锛?
### 5.3 -M npu锛堥鏈熷け璐ワ級
锛堝畬鏁磋緭鍑猴紝閲嶇偣锛氭敞鍐屽け璐ョ殑鎶ラ敊鍘熸枃锛?
缁撹锛氬け璐ュ師鍥犲垎鏋愶紙鍒濇鍗冲彲锛?

## 6. 寰呰В闂娓呭崟锛堟墽琛屼腑鍙戠幇鐨勪换浣曞紓甯革級
```

## 8. 瀹屾垚鏍囧織

鎶ュ憡鐢熸垚鍚庯紝澶栭儴鍥㈤槦闇€瑕佺殑鍥涙牱涓滆タ榻愪簡锛?
1. davinci/hmm 鍐呮牳绗﹀彿鏈夋棤鍙敤鐨勭粨璁?+ 鍘熷杈撳嚭
2. CANN dmabuf 鑳藉姏缁撹 + nm 鍘熷杈撳嚭
3. 缂栬瘧缁撴灉锛堟垚鍔熸垨瀹屾暣鎶ラ敊锛?
4. npu-staged 鍏ㄩ摼璺粨鏋?+ npu 璺嚎鎶ラ敊鍘熸枃

榻愪簡涔嬪悗 commit + push锛屽湪鍥炴墽鍖烘湯灏炬敞鏄庛€屽洓椤逛氦浠樺畬姣曪紝绛夊緟澶栭儴鎸囦护銆嶃€?

## 9. 鍐呴儴 AI 鍥炴墽鍖?

### 鍥炴墽瀵煎叆 2026-09-15锛堝閮?AI 浠ｅ綍锛?

鍐呴儴 AI 瀹屾暣鍥炴墽鐢辩敤鎴蜂互鏈湴鏂囦欢甯﹀洖锛堝閮ㄥ壇鏈?d:\NDS\nds_company_validation_task.md锛?
鍚叏閮ㄥ師濮嬭緭鍑猴級銆備互涓嬩粎褰曞叧閿簨瀹烇紝鍏ㄦ枃浠ョ敤鎴峰壇鏈负鍑嗭細

- 鏈哄櫒锛歯ode4=141.61.84.151锛圱esla V100锛?2 鍧?NVMe锛岀┖闂茬洏 nvme4n1/nvme7n1锛?
  绯荤粺鐩?nvme9n1锛夛紱node1=141.61.84.245锛圧TX 4090 D锛?1 鍧?NVMe锛?
  绌洪棽鐩?nvme4~11n1锛岀郴缁熺洏 nvme3n1锛夈€?*涓ゅ彴鍧囨棤 NPU/CANN/davinci**銆?
- 鍐呮牳 6.6.0锛坓dr_w00921547+ / netlab_pcie_ub_compat+锛夛紝openEuler 24.03 LTS-SP4锛?
  **AArch64**锛孶RMA 椹卞姩鏍堝凡鍔犺浇锛坲dma/ubcore/uburma/udma_nv_p2p_bridge 绛夛級銆?
- 缂栬瘧澶辫触锛歚CONFIG.sh: line 7: $'\r': command not found` 鈫?`Configuration failed`锛?
  EXIT=127锛圫FTP 涓婁紶鑷?CRLF 琛屽熬锛夛紱mk/config.mk 鏈敓鎴愩€?
- 浠ｇ爜浣嶇疆 /home/l00955908/nds/spdk锛圫FTP 涓婁紶鍚庨噸鏂?git init锛屽巻鍙查潪鍘熷 ddbd44a锛夈€?
- UMDK锛歯ode1 瀹屾暣婧愮爜鏍?/home/yin/gdr/UMDK_netlab锛坈onfigure 鍙瘑鍒級锛?
  node4 绯荤粺搴?/usr/lib64/liburma.so + 澶存枃浠?/home/xxx/urma_include銆?
- 閮ㄧ讲鏂囨。鏈壘鍒帮紱浣嗗彂鐜板悓浜婤 鐨?run_urma_perf_v6.sh锛坱rid锛歵raddr=141.61.84.151,
  trsvcid=4420, subnqn=nqn.2026-01.io.spdk:urma-gpu-test锛汥EV_NAME=udmac1d1e2锛?
  LD_PATH=/home/tong/umdk_gpu_isolated/lib锛変笌 target_nvme_takeover.sh 涓€閿剼鏈?
  锛堜袱鍙板潎瀛樺湪锛夈€?
- 鍥涢」浜や粯鐗╋細NPU 鐩稿叧涓夐」鏃犳硶浜や粯锛堟棤 NPU 鏈哄櫒锛夛紱缂栬瘧鍙楅樆寰呬慨銆?

## 10. 澶栭儴 AI 鎸囦护鍖?

### 鍥炴墽瀵煎叆 2026-09-15 #2锛堟壒娆?1 缁撴灉锛岀敤鎴峰甫鍥烇級

**197 浣撴涓冮」鍏ㄧ豢**锛欰 4脳 Ascend950PR锛?28GB HBM/鍗★級锛汢 CANN 绯荤粺绾у畨瑁?
锛堝鐗堟湰骞跺瓨锛夛紱C 10+ udmac 璁惧锛堟嫇鎵戞垚绔嬶級锛?*D 鍐呮牳瀛樺湪 vdavinci_pin_pages /
vdavinci_unpin_pages / hw_vdavinci_pin_page_range锛坉rv_vascend 妯″潡锛夆€斺€擯hase 2
鍐呮牳妗ユ帴鍙鎬х‘璁?*锛汦 CANN 鏈?aclrtMemExportToShareableHandle 绛夊鍑烘帴鍙?
锛堟槸鍚︾瓑浠?dma-buf fd 寰呮煡璇侊級锛汧 openEuler SP4 aarch64锛屼笌 151 寤惰繜 0.3ms銆?

**缂栬瘧澶辫触鏍瑰洜**锛?97 绯荤粺 liburma/澶存枃浠舵槸鏍囧噯鐗堬紝缂?gds 鎵╁睍
锛坲rma_seg_cfg_t.is_gpu_seg / urma_register_seg_dmabuf锛夆啋 make 澶辫触銆?
197 鍙洿鎺?GitHub clone锛堟棤 CRLF 闂锛夛紱245 鐨?gds 鐗?UMDK 鏍?
锛?home/yin/gdr/UMDK_netlab锛夊惈鎵€闇€鎵╁睍銆?

**鍐崇瓥**锛氱紪璇?娴嬭瘯鍏ㄩ儴鏀瑰湪 197 杩涜锛?45 閫€鍑猴紙CRLF 闂涓嶅啀澶勭悊锛夛紱
gds UMDK 浠?245 鎷疯礉鍒?197 鑷鐩綍闅旂浣跨敤锛堜笉鍔ㄧ郴缁熷簱锛夈€?

### 鍥炴墽瀵煎叆 2026-09-15 #3锛堟壒娆?2 缁撴灉锛岀敤鎴峰甫鍥烇級

- gds UMDK 宸叉嫹鑷?197 /home/lx/nds/UMDK_netlab锛坰shpass 涓浆锛夛紝gds 鎵╁睍楠岃瘉
  榻愬叏锛坕s_gpu_seg 瀛楁 + liburma.so 宸插鍑?urma_register_seg_dmabuf锛?
- configure 鎴愬姛锛汼PDK 鏍稿績搴?+ 12 涓?bin 鍏ㄩ儴缂栬瘧鎴愬姛锛坕s_gpu_seg 閿欒娑堝け锛?
- 鉂?urma_perf 缂栬瘧澶辫触锛歶rma_perf_npu.c 鐨?npu_driver_init/npu_driver_fini
  瀹氫箟涓?static锛屼笌澶存枃浠堕潪 static 澹版槑鍐茬獊鈥斺€?*澶栭儴寮€鍙?bug锛屽凡淇**
  锛坈ommit锛歠ix(nds) static 澹版槑鍐茬獊锛?
- **D 椤圭粨璁轰慨姝ｏ紙閲嶈锛?*锛歷davinci_pin_pages 绛夌鍙峰湪 kallsyms 涓潎涓哄皬鍐?
  t锛堟ā鍧楀眬閮級锛屾棤 __ksymtab鈥斺€?*鏈鍑猴紝Phase 2 鍐呮牳妗ユ帴涓嶈兘鐩存帴璋冪敤**銆?
  Phase 2 鏂瑰悜閲嶄及锛欵 椤癸紙CANN 瀵煎嚭鎺ュ彛 鈫?dma-buf 娉ㄥ唽璺嚎锛夐噸瑕佹€т笂鍗囷紝
  鍥?dma-buf 璧版爣鍑嗗唴鏍告鏋讹紝鏃犻渶 davinci 涓撴湁绗﹀彿锛涘緟鏌ヨ瘉鐐癸細gds 鍐呮牳
  udma 椹卞姩鐨?urma_register_seg_dmabuf 鏄惁宸插疄鐜?dma-buf import銆?

### 鎸囦护 2026-09-15 #6锛氳В閿佹壒娆?3锛坱arget 鍚姩 + cpu 鍥炲綊 + NDS 鍏ㄩ摼璺娴嬶級

鍓嶇疆锛氭壒娆?2 琛ヤ竵宸茬‘璁わ紙urma_perf 灏辩华锛夈€傞伒瀹?0.5 鑺傞殧绂诲畧鍒欍€?

**閲嶈鍙樺寲**锛?97 鏈?CANN锛宍-M npu-staged` 涓嶅啀鏄敊璇矾寰勯獙璇侊紝鑰屾槸
**NDS 鍏ㄩ摼璺寮忛娴?*锛圚BM鈫抙ost 鏆傚瓨鈫扤IC锛屼笉渚濊禆鍐呮牳 NPU 妗ユ帴锛夈€?

**3a. 鍚姩 target锛坣ode4/151锛?*

```bash
cd /home/xxx/spdk
grep -i huge /proc/meminfo              # 璺戝墠璁板綍锛堥殧绂诲畧鍒欑 4 鏉★級
./target_nvme_takeover.sh               # 鍙垎鏋愶紝纭鍊欓€夌洏鏃犲紓甯?
cd /home/xxx/spdk && ./target_nvme_takeover.sh -d nvme4n1   # 鎺ョ+鍚姩+RPC 閰嶇疆
```
- 鑻ュ垎鏋愮粨鏋滃紓甯告垨鑴氭湰鎶ラ敊锛氬仠涓嬶紝鍘熸牱鍥炰紶绛夊緟鎸囦护锛屼笉瑕佸己琛屾帴绠?
- 璁板綍鑴氭湰瀵圭郴缁熷仛鐨勬敼鍔紙vfio 缁戝畾銆乭ugepages銆丷PC 閰嶇疆锛変互渚挎仮澶?

**3b. 197 鏌ユ湰鏈?URMA 璁惧鍚?+ cpu 鍥炲綊**

```bash
find /sys -name '*udmac*' 2>/dev/null | head    # 璁板綍 197 鐨勮澶囧悕
LD_LIBRARY_PATH=/home/lx/nds/UMDK_netlab/lib \
SPDK_URMA_MAX_IO_SIZE=4194304 \
./build/examples/urma_perf \
  -r 'trtype:URMA adrfam:IPv4 traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' \
  -M cpu -t 5
```
- 鍏堜笉甯?SPDK_URMA_DEV_NAME锛堣嚜鍔ㄩ€夌涓€涓澶囷級锛涜嫢杩炴帴/鍒濆鍖栧け璐ワ紝
  琛?`SPDK_URMA_DEV_NAME=<197 鐨勮澶囧悕>` 閲嶈瘯涓€娆★紝涓ゆ杈撳嚭閮借褰?
- **鏈鍚屾椂棣栨楠岃瘉 gds liburma 涓?197 鍐呮牳椹卞姩鐨勫吋瀹规€?*鈥斺€攗rma_init
  鎴栬繛鎺ラ樁娈电殑鎶ラ敊鏄叧閿暟鎹紝鍘熸牱璁板綍
- 棰勬湡锛歚Preflight host WRITE + READ verification passed` + 甯?18/寤惰繜杈撳嚭

**3c. NDS 鍏ㄩ摼璺娴嬶紙-M npu-staged锛屼富鎴樺満锛?*

```bash
LD_LIBRARY_PATH=/home/lx/nds/UMDK_netlab/lib \
SPDK_URMA_MAX_IO_SIZE=4194304 \
./build/examples/urma_perf \
  -r 'trtype:URMA adrfam:IPv4 traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' \
  -M npu-staged -t 5
```
- 棰勬湡锛歚Preflight staged NPU->host WRITE + READ verification passed`銆?
  `mem_type=npu-staged`銆佸甫018/寤惰繜琛屻€乣staged_copy` 鑰楁椂琛?
- 鍏ㄩ儴鍘熸牱璁板綍锛沘clrt 鐩稿叧鎶ラ敊涔熸槸鍏抽敭鏁版嵁

**3d. -M npu锛坧eermem 璺嚎锛岄鏈熷け璐ワ紝鏀堕泦鎶ラ敊锛?*

```bash
LD_LIBRARY_PATH=/home/lx/nds/UMDK_netlab/lib \
./build/examples/urma_perf \
  -r 'trtype:URMA adrfam:IPv4 traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' \
  -M npu -t 1
```
- 棰勬湡鍦ㄥ唴瀛樻敞鍐岄樁娈靛け璐ワ紙is_gpu_seg 鍐呮牳璺嚎锛夛紝鎶ラ敊鍘熸枃鏄?Phase 2
  dma-buf 璺嚎璁捐鐨勭洿鎺ヨ緭鍏ワ紝鍔″繀瀹屾暣璁板綍

**3e. 鎭㈠鐜板満**

- node4锛氳繕鍘?NVMe 鐩橈紙鑴氭湰娈嬬暀杩樺師鍔熻兘锛涗笉纭畾鍒欒褰曠姸鎬佺瓑寰呮寚浠わ級
- 197锛氱‘璁?hugepages 鎭㈠鍒?3a 璺戝墠姘村钩锛屾竻鐞嗕复鏃舵枃浠?

**鍥炰紶**锛氬叏閮ㄨ緭鍑鸿拷鍔犵 9 鑺傛柊鏉＄洰锛屾敞鏄庛€屾壒娆?3 瀹屾瘯銆嶏紱
3c 鑻ラ€氳繃锛岃繖鏄?NDS 棣栨鍏ㄩ摼璺窇閫氾紝鍊煎緱鍦ㄧ粨璁洪噷鏄庣‘鍐欏嚭銆?

### 鎸囦护 2026-09-15 #5锛氭壒娆?2 琛ヤ竵锛堟媺淇閲嶇紪 urma_perf锛?

鍓嶇疆锛氭壒娆?2 鍏朵綑閮ㄥ垎宸插畬鎴愩€傛湰鎵瑰叏閮ㄥ湪 **197** 涓婏紝閬靛畧闅旂瀹堝垯銆?

```bash
cd /home/lx/nds/spdk          # 鎴栧疄闄?GitHub clone 鐩綍
git stash list | head -1      # 纭鏃犳湭淇濆瓨鏀瑰姩锛堟湁鍒欏厛鍥炰紶璇㈤棶锛?
git pull origin nds_v1        # 鎷夊彇 static 澹版槑淇
nice -n 10 make -j16 2>&1 | tail -20   # 澧為噺缂栬瘧锛屽彧閲嶇紪 urma_perf锛屽緢蹇?
ls -l build/examples/urma_perf
./build/examples/urma_perf -h 2>&1 | grep -A3 -- '-M'
```

**瀹屾垚鏍囧織**锛歶rma_perf 缂栧嚭銆佸府鍔╂枃鏈惈 npu/npu-staged 鈫?鍥炰紶娉ㄦ槑
銆屾壒娆?2 琛ヤ竵瀹屾瘯锛岀瓑寰呰В閿佹壒娆?3銆嶃€傝嫢 make 浠嶅け璐ワ紝瀹屾暣鎶ラ敊鍥炰紶銆?

### 鎸囦护 2026-09-15 #4锛氳В閿佹壒娆?2锛堝湪 197 鎼缓 gds 缂栬瘧鐜锛?

鍓嶇疆锛氭壒娆?1 宸茬‘璁ゃ€傛湰鎵瑰叏閮ㄥ湪 **197** 涓婃搷浣滐紝閬靛畧 0.5 鑺傞殧绂诲畧鍒欍€?
245 涓婄殑 CRLF 淇浠诲姟浣滃簾銆?

**2a. 浠?245 鎷疯礉 gds 鐗?UMDK 鏍戝埌鑷鐩綍锛堝彧璇?245锛夛細**

```bash
# 鍦?197 涓婃墽琛岋紙鑻?197 涓?245 ssh 涓嶉€氾紝鏀圭敱鐢ㄦ埛涓浆鎷疯礉锛?
mkdir -p /home/l00955908/nds
scp -r /home/yin/gdr/UMDK_netlab /home/l00955908/nds/UMDK_netlab
```

**2b. 楠岃瘉鎷疯礉鐗╁惈 gds 鎵╁睍锛堝彧璇伙級锛?*

```bash
# 澶存枃浠跺簲鏈?is_gpu_seg 瀛楁
timeout 30 find /home/l00955908/nds/UMDK_netlab -name "urma_api.h" | head -3
grep -rn "is_gpu_seg" /home/l00955908/nds/UMDK_netlab/src/urma/lib/urma/core/include/ | head -5
# 搴撳簲鏈?urma_register_seg_dmabuf 瀵煎嚭
nm -D /home/l00955908/nds/UMDK_netlab/lib/liburma.so | grep -E "register_seg"
```

**2c. 閲嶆柊 configure + make锛?97 涓?GitHub clone 鐨?spdk 鐩綍锛夛細**

```bash
cd /home/l00955908/nds/spdk   # 鎴栧疄闄?clone 鐩綍
git log --oneline -3           # 搴旂湅鍒?cc53791/3bf02d6/e1b585f/ddbd44a 绯诲垪
./configure --with-urma=/home/l00955908/nds/UMDK_netlab 2>&1 | tail -20
nice -n 10 make -j16 2>&1 | tail -50
ls -l build/examples/urma_perf
./build/examples/urma_perf -h 2>&1 | grep -A3 -- '-M'
```

**2d. 銆愭柊澧烇紝鍙銆戣ˉ榻?D/E 涓ら」鐨勭粏鑺傦紙Phase 2 璁捐杈撳叆锛夛細**

```bash
# D 缁嗚妭锛歞rv_vascend 妯″潡鐨?pin 绗﹀彿鏄惁瀵瑰瀵煎嚭锛圱=瀵煎嚭 / t=灞€閮級
timeout 60 find /lib/modules/$(uname -r) -name 'drv_vascend*' 2>/dev/null | head -3
nm <涓婇潰鎵惧埌鐨?.ko 璺緞> 2>/dev/null | grep -i vdavinci | head -10
# E 缁嗚妭锛欳ANN 瀵煎嚭鎺ュ彛瀹屾暣绛惧悕绾跨储
nm -D /usr/local/Ascend/ascend-toolkit/latest/lib64/libascendcl.so 2>/dev/null \
  | grep -iE "Export|Shareable|MallocPhysical|ImportFrom" | head -20
#锛堣嫢 latest 閾炬帴涓嶅瓨鍦紝鐢ㄦ壒娆?1 鎵惧埌鐨勫疄闄呯増鏈矾寰勶級
```

**瀹屾垚鏍囧織**锛歶rma_perf 缂栧嚭涓斿府鍔╂枃鏈惈 npu/npu-staged 鈫?鍥炰紶娉ㄦ槑
銆屾壒娆?2 瀹屾瘯锛岀瓑寰呰В閿佹壒娆?3銆嶃€傝嫢 make 浠嶅け璐ワ紝瀹屾暣鎶ラ敊鍥炰紶绛夊緟鎸囦护銆?

**鎻愮ず**锛氫箣鍚庢墍鏈?urma_perf 杩愯閮藉甫
`LD_LIBRARY_PATH=/home/l00955908/nds/UMDK_netlab/lib`锛堥殧绂诲姞杞?gds liburma锛?
涓嶅姩绯荤粺搴擄級锛沢ds liburma 涓?197 鍐呮牳椹卞姩鐨勫吋瀹规€у皢鍦ㄦ壒娆?3 棣栨杩炴帴鏃堕獙璇併€?

### 鎸囦护 2026-09-15 #3锛氭敼鍒嗘壒鍒讹紝鏈疆鍙仛鎵规 1

澶栭儴纭锛氫换鍔＄矑搴﹀お澶э紝鏀逛负**鍒嗘壒鎵ц銆佹瘡鎵瑰洖浼犮€佺‘璁ゅ悗瑙ｉ攣涓嬩竴鎵?*銆?
鏈潯鎸囦护**鍙栦唬**涔嬪墠鎸囦护涓殑鎵ц鑺傚锛堝悇鎵规鍐呭涓嶅彉锛夛細

| 鎵规 | 鍐呭 | 椋庨櫓 | 瑙ｉ攣鏉′欢 |
|---|---|---|---|
| **鎵规 1锛堟湰杞級** | 197 鑺傜偣浣撴 A~F 涓冮」锛堟寚浠?#2 娓呭崟锛屽叏閮ㄥ彧璇伙級 | 闆堕闄?| 鍗冲埢鎵ц |
| 鎵规 2 | 淇?CRLF + 缂栬瘧 urma_perf锛堟寚浠?#1 浠诲姟 1锛?| 浣庯紙浜х墿鍦ㄨ嚜瀹剁洰褰曪級 | 鎵规 1 鍥炰紶纭鍚?|
| 鎵规 3 | target 鍚姩 + -M cpu 鍥炲綊 + 閿欒璺緞锛堜换鍔?2/3锛?| 涓紙鎺ョ 151 鐨勭洏锛?| 鎵规 2 鍥炰紶纭鍚?|
| 鎵规 4 | 鎭㈠鐜板満锛堜换鍔?4锛?| 浣?| 鎵规 3 瀹屾垚 |

**鏈疆浣犲彧鍋?*锛氭寚浠?#2 鐨?NPU 鑺傜偣浣撴 A~F 涓冮」锛堝惈 B2锛夛紝鍘熸牱璁板綍鍏ㄩ儴杈撳嚭锛?
杩藉姞鍒扮 9 鑺傚洖鎵у尯鍚庡洖浼狅紝娉ㄦ槑銆屾壒娆?1 瀹屾瘯锛岀瓑寰呰В閿佹壒娆?2銆嶃€?
**涓嶈**寮€濮嬫壒娆?2/3 鐨勪换浣曟搷浣滐紙鍖呮嫭淇?CRLF銆佺紪璇戙€佹帴绠＄洏锛夈€?

### 鎸囦护 2026-09-15 #1锛堝凡鏀瑰垎鎵瑰埗锛屽唴瀹逛繚鐣欏鏌ワ級

鑳屾櫙璇存槑锛氳繖涓ゅ彴鏄悓浜婤 鐨?GDS 娴嬭瘯鏈猴紙151/245锛夛紝鏃?NPU 灞炲疄銆傛湰杞洰鏍囨敼涓?
**銆屼唬鐮佽川閲忛獙璇併€?*锛氫慨缂栬瘧 鈫?CPU 鍥炲綊 鈫?閿欒璺緞銆侼PU 鐩稿叧娴嬭瘯绛夌湡姝ｇ殑
鏄囪吘鏈哄櫒鍒颁綅鍚庡啀鍋氾紙鐢ㄦ埛姝ｅ湪鍗忚皟锛夈€?

**浠诲姟 1锛氫慨澶?CRLF 骞跺畬鎴愮紪璇戯紙node1 = 141.61.84.245锛?*

```bash
# 1a. 鍏堟祴 GitHub 杩為€氭€?
curl -sI --max-time 10 https://github.com | head -3
```

- **鑻ラ€?*锛氭崲鍒版柊鐩綍閲嶆柊 clone锛堥『甯﹁В鍐?git 鍘嗗彶闂锛夛細
  `git clone -b nds_v1 https://github.com/yyyuanhao426-hash/spdk.git /home/l00955908/nds/spdk-git`
  鐒跺悗鍦ㄦ柊鐩綍缂栬瘧锛涢獙璇?`git log --oneline -3` 搴旂湅鍒?e1b585f / ddbd44a銆?
- **鑻ヤ笉閫?*锛氬湪鐜版湁鐩綍杞崲琛屽熬锛坒ile 妫€娴嬪彧杞?CRLF 鏂囦欢锛屼笉鍔ㄤ簩杩涘埗锛夛細
  ```bash
  cd /home/l00955908/nds/spdk
  git ls-files -z | xargs -0 file | grep CRLF | cut -d: -f1 | xargs -r sed -i 's/\r$//'
  ```

鐒跺悗缂栬瘧骞堕獙璇侊細

```bash
./configure --with-urma=/home/yin/gdr/UMDK_netlab 2>&1 | tail -20
make -j$(nproc) 2>&1 | tail -50
ls -l build/examples/urma_perf
./build/examples/urma_perf -h 2>&1 | grep -A2 -- '-M'
```

棰勬湡锛歝onfigure/make 鎴愬姛锛涘府鍔╂枃鏈腑鍑虹幇 npu / npu-staged 鍙栧€笺€?

**浠诲姟 2锛?M cpu 鍥炲綊锛堝厛鍚姩 target锛?*

2a. 鍦?**node4锛坱arget锛?51锛?*锛氬厛鍙垎鏋愪笉鎺ョ锛?
```bash
cd /home/xxx/spdk && ./target_nvme_takeover.sh        # 鍙垎鏋愶紝璁板綍鍊欓€夌洏
cd /home/xxx/spdk && ./target_nvme_takeover.sh -d nvme4n1   # 鐢ㄧ┖闂茬洏鎺ョ+鍚姩 nvmf_tgt
```
锛堟帴绠″墠鏍稿 nvme4n1 纭负绌洪棽鐩橈紱鑻ヨ剼鏈垎鏋愮粨鏋滄樉绀哄紓甯革紝鍋滀笅鍥炰紶绛夊緟鎸囦护锛?

2b. 鍦?**node1锛坕nitiator锛?45锛?*锛氬厛纭鏈満 UMDK lib 璺緞瀛樺湪锛?
```bash
ls /home/tong/umdk_gpu_isolated/lib /home/yin/gdr/UMDK_netlab/lib 2>/dev/null
```
鐒跺悗鍏堟煡鏈満 URMA 璁惧鍚嶏紙鍚屼簨B 鑴氭湰閲岀殑 udmac1d1e2 鏄棫 initiator node3 鐨勶紝
node1 鍙兘涓嶅悓锛夛細
```bash
find /sys -name '*udmac*' 2>/dev/null | head
ls /dev | grep -i udma
```
鍐嶈窇鍥炲綊锛堝厛涓嶅甫 SPDK_URMA_DEV_NAME 璁╁叾鑷姩閫夌涓€涓澶囷紱濡傝繛鎺ュけ璐ュ啀
琛ヤ笂 DEV_NAME 閲嶈瘯锛夈€?*閬靛畧闅旂瀹堝垯**锛氳窇鍓嶈褰?hugepages 鐜扮姸
锛坄grep -i huge /proc/meminfo`锛夛紝榛樿 -T 1 鍙粦 1 鏍告棤闇€ -m锛屽嬁鍔犲ぇ绾跨▼鏁帮細
```bash
LD_LIBRARY_PATH=<涓婇潰瀛樺湪鐨?umdk lib 璺緞> \
SPDK_URMA_MAX_IO_SIZE=4194304 \
./build/examples/urma_perf \
  -r 'trtype:URMA adrfam:IPv4 traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' \
  -M cpu -t 5
```
棰勬湡锛歚Preflight host WRITE + READ verification passed` + 甯﹀/寤惰繜杈撳嚭銆?

**浠诲姟 3锛氶敊璇矾寰勯獙璇侊紙node1锛屼笉闇€瑕?target锛?*

```bash
./build/examples/urma_perf -r 'trtype:URMA adrfam:IPv4 traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' -M npu -t 1
./build/examples/urma_perf -r '...' -M npu-staged -t 1
```
棰勬湡鍧囨姤 `Unable to load libascendcl.so`锛堟棤 CANN 鐜浼橀泤澶辫触锛夆€斺€?
**鍘熸牱璁板綍鎶ラ敊**锛岃繖鏄敊璇鐞嗚矾寰勭殑楠岃瘉銆?

**浠诲姟 4锛氭仮澶嶇幇鍦?*

- node4锛氭祴璇曞悗杩樺師 NVMe 鐩橈紙鑴氭湰鏈夋畫鐣欒繕鍘熷姛鑳斤紱鑻ヤ笉纭畾杩樺師鏂规硶锛?
  璁板綍鐩樼殑褰撳墠鐘舵€佸苟娉ㄦ槑"绛夊緟杩樺師鎸囦护"锛屼笉瑕佽嚜琛屾搷浣滐級
- node1/197锛氱‘璁?hugepages 鎭㈠鍒拌窇鍓嶆按骞筹紙瀵圭収浠诲姟 2b 鐨勮褰曪級锛?
  娓呯悊鑷繁鐩綍涓嬬殑涓存椂鏂囦欢锛屼粬浜虹洰褰曚笌绯荤粺鐩綍涓嶅姩

**鍥炰紶瑕佹眰**锛氬叏閮ㄨ緭鍑鸿拷鍔犲埌鏈枃浠剁 9 鑺傛柊鏉＄洰锛宑ommit
锛坄docs(nds-task): CRLF淇+缂栬瘧+cpu鍥炲綊+閿欒璺緞楠岃瘉`锛夊苟 push锛?
鑻ユ満鍣ㄦ棤娉曡闂?GitHub锛屾妸杩藉姞鍚庣殑鏈枃浠跺叏鏂囦氦缁欑敤鎴峰甫鍥炪€?

### 鎸囦护 2026-09-15 #2锛堝閮?AI 鈫?鍐呴儴 AI锛夛細鏂板 NPU 鑺傜偣浣撴

鑳屾櫙锛氱敤鎴风‘璁ゅ叕鍙告湁涓€鍙版槆鑵?NPU 鑺傜偣锛堝敮涓€涓€鍙帮級锛岃鍒掍綔涓?NDS 鐨?
Initiator锛汿arget 缁х画鐢?node4/151銆傛湰杞湪瀹屾垚涓婁竴杞寚浠ょ殑鍚屾椂锛?
**浼樺厛**瀵硅繖鍙?NPU 鑺傜偣鍋氱幆澧冧綋妫€锛堝畠鑳藉洖绛?Phase 2 鐨勪袱涓叧閿棶棰橈級銆?

**NPU 鑺傜偣浣撴娓呭崟锛堝湪璇ヨ妭鐐逛笂閫愭潯鎵ц骞跺師鏍疯褰曪級锛?*

```bash
# A. NPU 鍩虹
npu-smi info

# B. CANN 鏄惁瀹夎锛堥檺瀹氳矾寰勶紝绂佹鍏ㄧ洏 find锛?
timeout 60 find /usr/local/Ascend /usr/lib64 /opt /home -name "libascendcl.so" 2>/dev/null
ls /usr/local/Ascend 2>/dev/null

# B2. UMDK/liburma 鏄惁鍙敤锛堢紪璇?urma_perf 蹇呴渶锛涢檺瀹氳矾寰勶級
timeout 60 find /usr/local /usr/lib64 /opt /home -name "urma_api.h" -not -path "*/spdk*" 2>/dev/null
ldconfig -p | grep urma
ls /usr/lib64/liburma* 2>/dev/null

# C. 銆愬喅瀹氭垚璐ャ€慤RMA 缃戝崱鏄惁瀛樺湪
find /sys -name '*udmac*' 2>/dev/null | head
ls /dev | grep -iE "udma|ub"
lsmod | grep -E "udma|urma|ubus|ubcore|ubase"

# D. 銆怭hase 2 鍏抽敭銆戝唴鏍?davinci pin 绗﹀彿
cat /proc/kallsyms | grep -i davinci | head -50
cat /proc/kallsyms | grep -iE "hmm_|davinci.*pin|pin.*davinci" | head -30

# E. 銆怭hase 2 鍏抽敭銆慍ANN dmabuf 瀵煎嚭鑳藉姏锛堢敤 B 鐨勮矾寰勪唬鍏ワ級
nm -D <libascendcl.so 璺緞> | grep -iE "dmabuf|handle|fd|export" | head -40

# F. 鍐呮牳/绯荤粺涓庣綉缁滆繛閫氭€?
uname -r
cat /etc/os-release | head -3
uname -m
ip a | grep "inet "
# 涓?node4/151 杩為€氭€э紙TCP + ping锛?
ping -c 3 141.61.84.151
```

**鍒ゅ畾鏍囧噯锛堝啓鍏ユ姤鍛婄粨璁猴級锛?*
- A~C 鍏ㄩ儴姝ｅ父 鈫?NPU 鑺傜偣鍙洿鎺ュ綋 Initiator锛孨DS 鍏ㄩ摼璺祴璇曞彲鎺掓湡
  锛堟嫇鎵戝畾涓?197=Initiator + 151=Target锛?45 浠呭湪鏈疆 CPU 鍥炲綊涓綋缂栬瘧鏈猴紝
  NDS 姝ｅ紡娴嬭瘯涓嶅啀闇€瑕侊級
- C 鏃?URMA 璁惧 鈫?纭欢缂哄彛锛屽洖鎶?闇€鍗忚皟 URMA 缃戝崱"锛屽叾浣欑収甯镐氦浠?
- B2 鏃?UMDK/liburma 鈫?缂栬瘧缂哄彛锛屽彲鍦?197 涓婅 UMDK 鎴栨妸 245 缂栧ソ鐨?
  浜岃繘鍒舵嫹璐濊繃鍘伙紝鍥炴姤鏃舵敞鏄庨€夋嫨鍝鏂瑰紡
- D/E 鐨勮緭鍑烘槸 Phase 2 鍐呮牳妗ユ帴妯″潡璁捐鐨勭洿鎺ヨ緭鍏ワ紝鍔″繀鍘熸枃璁板綍
