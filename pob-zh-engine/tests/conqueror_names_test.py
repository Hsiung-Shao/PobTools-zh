# 征服者中文名要與 GGPK 珠寶詞(stat_descriptions.txt 永恆珠寶模板)一致
# 用法:python tests/conqueror_names_test.py
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# GGPK 3.29.3 stat_descriptions.txt 的永恆珠寶模板裡出現的譯名(查證 2026-11-05)
GGPK = {
    "Xibaqua": "賽巴昆", "Zerphi": "澤佛伊", "Ahuana": "阿呼阿娜", "Doryani": "多里亞尼",
    "Kaom": "岡姆", "Rakiata": "萊基亞塔", "Kiloava": "基洛阿瓦", "Akoya": "艾古亞",
    "Deshret": "迪虛瑞特", "Balbala": "貝爾巴拉", "Asenath": "安賽娜絲", "Nasima": "納西瑪",
    "Venarius": "維那利斯", "Maxarius": "瑪薩里歐斯", "Dominus": "神主", "Avarius": "伊爾莉斯",
    "Cadiro": "卡迪羅", "Victario": "維多里奧", "Chitus": "切特斯", "Caspiro": "卡斯皮羅",
    "Vorana": "沃拉娜", "Uhtred": "烏特雷", "Medved": "梅德偉",
    "Tecrod": "特克羅德", "Ulaman": "烏拉曼", "Kurgal": "柯戈", "Amanamu": "阿姆那姆", "Zorath": "澤洛斯",
}

fails = 0
checks = 0


def check(ok, msg):
    global fails, checks
    checks += 1
    if not ok:
        fails += 1
        print("FAIL", msg)


def load(p):
    with open(os.path.join(ROOT, p), encoding="utf-8") as f:
        return json.load(f)


# C1 軍團珠寶計算器的征服者名 = GGPK
tj = load("host/data/timeless_jewels.json")
seen = set()
for lst in tj["conquerors"].values():
    for c in lst:
        seen.add(c["name"])
        check(c["nameZh"] == GGPK.get(c["name"]), f"C1 timeless_jewels {c['name']}: {c['nameZh']} != {GGPK.get(c['name'])}")
check(seen == set(GGPK), f"C1 征服者清單不符:{sorted(seen ^ set(GGPK))}")

# POB Classes/TreeTab.lua conquerorTypes 的下拉標籤;基石名 = GGPK alternatepassiveskills.Name
KEYSTONE = {
    "Doryani (Corrupted Soul)": "腐化之魂", "Xibaqua (Divine Flesh)": "神聖血肉",
    "Ahuana (Immortal Ambition)": "不朽野望", "Kaom (Strength of Blood)": "血之力量",
    "Rakiata (Tempered by War)": "戰爭淬煉", "Akoya (Chainbreaker)": "除鏈師",
    "Asenath (Dance with Death)": "與死共舞", "Nasima (Second Sight)": "先見之明",
    "Balbala (The Traitor)": "叛徒", "Avarius (Power of Purpose)": "力量之意",
    "Dominus (Inner Conviction)": "內觀信念", "Maxarius (Transcendence)": "超凡脫俗",
    "Cadiro (Supreme Decadence)": "至高．頹廢", "Victario (Supreme Grandstanding)": "至高．譁眾",
    "Caspiro (Supreme Ostentation)": "至高．炫耀", "Vorana (Black Scythe Training)": "黑鐮之訓",
    "Uhtred (Celestial Mathematics)": "星界計算", "Medved (The Unbreaking Circle)": "不滅之環",
    "Tecrod (Overwhelming Hate)": "壓倒性恨意", "Ulaman (Weighted Exchange)": "加重交換",
    "Kurgal (Reconstructed Essence)": "重構精髓", "Amanamu (The Loyal Few)": "忠誠少數",
}

# C2 POB 查詢永恆珠寶下拉每一條都有譯文,且為「GGPK 征服者名: GGPK 基石名」
ui = load("dist/Data/poe1/zh-rTW/ui.json")
flat = {}
for v in ui.values():
    if isinstance(v, dict):
        flat.update(v)
for en, ks in KEYSTONE.items():
    want = GGPK[re.match(r"[A-Z][a-z]+", en).group(0)] + ": " + ks
    check(flat.get(en) == want, f"C2 ui.json {en}: {flat.get(en)} != {want}")
# Zorath 的下拉只有名字、沒有基石
check(flat.get("Zorath") == GGPK["Zorath"], f"C2 ui.json Zorath: {flat.get('Zorath')}")

print(f"{checks} checks, {fails} failed")
sys.exit(1 if fails else 0)
