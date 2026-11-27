/*
 * SC2 portable core: BIP-39 English word list (2048 words), used for the
 * safety-number word form.
 *
 * Source: https://github.com/bitcoin/bips/blob/master/bip-0039/english.txt
 * (as shipped by trezor/python-mnemonic, MIT licence,
 * Copyright (c) 2013-2016 Pavol Rusnak). SHA-256 of the newline-terminated list:
 * 2f5eed53a4727b4bf8880d8f3f199efc90e58503646d9ff8eff3a2ed3b24dbda
 * SPDX-License-Identifier: MIT
 */
#include "sc2_safety.h"

#include <string.h>

#define WORD_GROUP_LENGTH 16U

/* Each group contains sixteen NUL-terminated words. Index the groups
 * directly, then scan up to fifteen words to find the requested entry. */
static const char *const WORD_GROUPS[] = {
    "abandon\0" "ability\0" "able\0" "about\0" "above\0" "absent\0" "absorb\0" "abstract\0"
    "absurd\0" "abuse\0" "access\0" "accident\0" "account\0" "accuse\0" "achieve\0" "acid",
    "acoustic\0" "acquire\0" "across\0" "act\0" "action\0" "actor\0" "actress\0" "actual\0"
    "adapt\0" "add\0" "addict\0" "address\0" "adjust\0" "admit\0" "adult\0" "advance",
    "advice\0" "aerobic\0" "affair\0" "afford\0" "afraid\0" "again\0" "age\0" "agent\0"
    "agree\0" "ahead\0" "aim\0" "air\0" "airport\0" "aisle\0" "alarm\0" "album",
    "alcohol\0" "alert\0" "alien\0" "all\0" "alley\0" "allow\0" "almost\0" "alone\0"
    "alpha\0" "already\0" "also\0" "alter\0" "always\0" "amateur\0" "amazing\0" "among",
    "amount\0" "amused\0" "analyst\0" "anchor\0" "ancient\0" "anger\0" "angle\0" "angry\0"
    "animal\0" "ankle\0" "announce\0" "annual\0" "another\0" "answer\0" "antenna\0" "antique",
    "anxiety\0" "any\0" "apart\0" "apology\0" "appear\0" "apple\0" "approve\0" "april\0"
    "arch\0" "arctic\0" "area\0" "arena\0" "argue\0" "arm\0" "armed\0" "armor",
    "army\0" "around\0" "arrange\0" "arrest\0" "arrive\0" "arrow\0" "art\0" "artefact\0"
    "artist\0" "artwork\0" "ask\0" "aspect\0" "assault\0" "asset\0" "assist\0" "assume",
    "asthma\0" "athlete\0" "atom\0" "attack\0" "attend\0" "attitude\0" "attract\0" "auction\0"
    "audit\0" "august\0" "aunt\0" "author\0" "auto\0" "autumn\0" "average\0" "avocado",
    "avoid\0" "awake\0" "aware\0" "away\0" "awesome\0" "awful\0" "awkward\0" "axis\0"
    "baby\0" "bachelor\0" "bacon\0" "badge\0" "bag\0" "balance\0" "balcony\0" "ball",
    "bamboo\0" "banana\0" "banner\0" "bar\0" "barely\0" "bargain\0" "barrel\0" "base\0"
    "basic\0" "basket\0" "battle\0" "beach\0" "bean\0" "beauty\0" "because\0" "become",
    "beef\0" "before\0" "begin\0" "behave\0" "behind\0" "believe\0" "below\0" "belt\0"
    "bench\0" "benefit\0" "best\0" "betray\0" "better\0" "between\0" "beyond\0" "bicycle",
    "bid\0" "bike\0" "bind\0" "biology\0" "bird\0" "birth\0" "bitter\0" "black\0"
    "blade\0" "blame\0" "blanket\0" "blast\0" "bleak\0" "bless\0" "blind\0" "blood",
    "blossom\0" "blouse\0" "blue\0" "blur\0" "blush\0" "board\0" "boat\0" "body\0"
    "boil\0" "bomb\0" "bone\0" "bonus\0" "book\0" "boost\0" "border\0" "boring",
    "borrow\0" "boss\0" "bottom\0" "bounce\0" "box\0" "boy\0" "bracket\0" "brain\0"
    "brand\0" "brass\0" "brave\0" "bread\0" "breeze\0" "brick\0" "bridge\0" "brief",
    "bright\0" "bring\0" "brisk\0" "broccoli\0" "broken\0" "bronze\0" "broom\0" "brother\0"
    "brown\0" "brush\0" "bubble\0" "buddy\0" "budget\0" "buffalo\0" "build\0" "bulb",
    "bulk\0" "bullet\0" "bundle\0" "bunker\0" "burden\0" "burger\0" "burst\0" "bus\0"
    "business\0" "busy\0" "butter\0" "buyer\0" "buzz\0" "cabbage\0" "cabin\0" "cable",
    "cactus\0" "cage\0" "cake\0" "call\0" "calm\0" "camera\0" "camp\0" "can\0"
    "canal\0" "cancel\0" "candy\0" "cannon\0" "canoe\0" "canvas\0" "canyon\0" "capable",
    "capital\0" "captain\0" "car\0" "carbon\0" "card\0" "cargo\0" "carpet\0" "carry\0"
    "cart\0" "case\0" "cash\0" "casino\0" "castle\0" "casual\0" "cat\0" "catalog",
    "catch\0" "category\0" "cattle\0" "caught\0" "cause\0" "caution\0" "cave\0" "ceiling\0"
    "celery\0" "cement\0" "census\0" "century\0" "cereal\0" "certain\0" "chair\0" "chalk",
    "champion\0" "change\0" "chaos\0" "chapter\0" "charge\0" "chase\0" "chat\0" "cheap\0"
    "check\0" "cheese\0" "chef\0" "cherry\0" "chest\0" "chicken\0" "chief\0" "child",
    "chimney\0" "choice\0" "choose\0" "chronic\0" "chuckle\0" "chunk\0" "churn\0" "cigar\0"
    "cinnamon\0" "circle\0" "citizen\0" "city\0" "civil\0" "claim\0" "clap\0" "clarify",
    "claw\0" "clay\0" "clean\0" "clerk\0" "clever\0" "click\0" "client\0" "cliff\0"
    "climb\0" "clinic\0" "clip\0" "clock\0" "clog\0" "close\0" "cloth\0" "cloud",
    "clown\0" "club\0" "clump\0" "cluster\0" "clutch\0" "coach\0" "coast\0" "coconut\0"
    "code\0" "coffee\0" "coil\0" "coin\0" "collect\0" "color\0" "column\0" "combine",
    "come\0" "comfort\0" "comic\0" "common\0" "company\0" "concert\0" "conduct\0" "confirm\0"
    "congress\0" "connect\0" "consider\0" "control\0" "convince\0" "cook\0" "cool\0" "copper",
    "copy\0" "coral\0" "core\0" "corn\0" "correct\0" "cost\0" "cotton\0" "couch\0"
    "country\0" "couple\0" "course\0" "cousin\0" "cover\0" "coyote\0" "crack\0" "cradle",
    "craft\0" "cram\0" "crane\0" "crash\0" "crater\0" "crawl\0" "crazy\0" "cream\0"
    "credit\0" "creek\0" "crew\0" "cricket\0" "crime\0" "crisp\0" "critic\0" "crop",
    "cross\0" "crouch\0" "crowd\0" "crucial\0" "cruel\0" "cruise\0" "crumble\0" "crunch\0"
    "crush\0" "cry\0" "crystal\0" "cube\0" "culture\0" "cup\0" "cupboard\0" "curious",
    "current\0" "curtain\0" "curve\0" "cushion\0" "custom\0" "cute\0" "cycle\0" "dad\0"
    "damage\0" "damp\0" "dance\0" "danger\0" "daring\0" "dash\0" "daughter\0" "dawn",
    "day\0" "deal\0" "debate\0" "debris\0" "decade\0" "december\0" "decide\0" "decline\0"
    "decorate\0" "decrease\0" "deer\0" "defense\0" "define\0" "defy\0" "degree\0" "delay",
    "deliver\0" "demand\0" "demise\0" "denial\0" "dentist\0" "deny\0" "depart\0" "depend\0"
    "deposit\0" "depth\0" "deputy\0" "derive\0" "describe\0" "desert\0" "design\0" "desk",
    "despair\0" "destroy\0" "detail\0" "detect\0" "develop\0" "device\0" "devote\0" "diagram\0"
    "dial\0" "diamond\0" "diary\0" "dice\0" "diesel\0" "diet\0" "differ\0" "digital",
    "dignity\0" "dilemma\0" "dinner\0" "dinosaur\0" "direct\0" "dirt\0" "disagree\0" "discover\0"
    "disease\0" "dish\0" "dismiss\0" "disorder\0" "display\0" "distance\0" "divert\0" "divide",
    "divorce\0" "dizzy\0" "doctor\0" "document\0" "dog\0" "doll\0" "dolphin\0" "domain\0"
    "donate\0" "donkey\0" "donor\0" "door\0" "dose\0" "double\0" "dove\0" "draft",
    "dragon\0" "drama\0" "drastic\0" "draw\0" "dream\0" "dress\0" "drift\0" "drill\0"
    "drink\0" "drip\0" "drive\0" "drop\0" "drum\0" "dry\0" "duck\0" "dumb",
    "dune\0" "during\0" "dust\0" "dutch\0" "duty\0" "dwarf\0" "dynamic\0" "eager\0"
    "eagle\0" "early\0" "earn\0" "earth\0" "easily\0" "east\0" "easy\0" "echo",
    "ecology\0" "economy\0" "edge\0" "edit\0" "educate\0" "effort\0" "egg\0" "eight\0"
    "either\0" "elbow\0" "elder\0" "electric\0" "elegant\0" "element\0" "elephant\0" "elevator",
    "elite\0" "else\0" "embark\0" "embody\0" "embrace\0" "emerge\0" "emotion\0" "employ\0"
    "empower\0" "empty\0" "enable\0" "enact\0" "end\0" "endless\0" "endorse\0" "enemy",
    "energy\0" "enforce\0" "engage\0" "engine\0" "enhance\0" "enjoy\0" "enlist\0" "enough\0"
    "enrich\0" "enroll\0" "ensure\0" "enter\0" "entire\0" "entry\0" "envelope\0" "episode",
    "equal\0" "equip\0" "era\0" "erase\0" "erode\0" "erosion\0" "error\0" "erupt\0"
    "escape\0" "essay\0" "essence\0" "estate\0" "eternal\0" "ethics\0" "evidence\0" "evil",
    "evoke\0" "evolve\0" "exact\0" "example\0" "excess\0" "exchange\0" "excite\0" "exclude\0"
    "excuse\0" "execute\0" "exercise\0" "exhaust\0" "exhibit\0" "exile\0" "exist\0" "exit",
    "exotic\0" "expand\0" "expect\0" "expire\0" "explain\0" "expose\0" "express\0" "extend\0"
    "extra\0" "eye\0" "eyebrow\0" "fabric\0" "face\0" "faculty\0" "fade\0" "faint",
    "faith\0" "fall\0" "false\0" "fame\0" "family\0" "famous\0" "fan\0" "fancy\0"
    "fantasy\0" "farm\0" "fashion\0" "fat\0" "fatal\0" "father\0" "fatigue\0" "fault",
    "favorite\0" "feature\0" "february\0" "federal\0" "fee\0" "feed\0" "feel\0" "female\0"
    "fence\0" "festival\0" "fetch\0" "fever\0" "few\0" "fiber\0" "fiction\0" "field",
    "figure\0" "file\0" "film\0" "filter\0" "final\0" "find\0" "fine\0" "finger\0"
    "finish\0" "fire\0" "firm\0" "first\0" "fiscal\0" "fish\0" "fit\0" "fitness",
    "fix\0" "flag\0" "flame\0" "flash\0" "flat\0" "flavor\0" "flee\0" "flight\0"
    "flip\0" "float\0" "flock\0" "floor\0" "flower\0" "fluid\0" "flush\0" "fly",
    "foam\0" "focus\0" "fog\0" "foil\0" "fold\0" "follow\0" "food\0" "foot\0"
    "force\0" "forest\0" "forget\0" "fork\0" "fortune\0" "forum\0" "forward\0" "fossil",
    "foster\0" "found\0" "fox\0" "fragile\0" "frame\0" "frequent\0" "fresh\0" "friend\0"
    "fringe\0" "frog\0" "front\0" "frost\0" "frown\0" "frozen\0" "fruit\0" "fuel",
    "fun\0" "funny\0" "furnace\0" "fury\0" "future\0" "gadget\0" "gain\0" "galaxy\0"
    "gallery\0" "game\0" "gap\0" "garage\0" "garbage\0" "garden\0" "garlic\0" "garment",
    "gas\0" "gasp\0" "gate\0" "gather\0" "gauge\0" "gaze\0" "general\0" "genius\0"
    "genre\0" "gentle\0" "genuine\0" "gesture\0" "ghost\0" "giant\0" "gift\0" "giggle",
    "ginger\0" "giraffe\0" "girl\0" "give\0" "glad\0" "glance\0" "glare\0" "glass\0"
    "glide\0" "glimpse\0" "globe\0" "gloom\0" "glory\0" "glove\0" "glow\0" "glue",
    "goat\0" "goddess\0" "gold\0" "good\0" "goose\0" "gorilla\0" "gospel\0" "gossip\0"
    "govern\0" "gown\0" "grab\0" "grace\0" "grain\0" "grant\0" "grape\0" "grass",
    "gravity\0" "great\0" "green\0" "grid\0" "grief\0" "grit\0" "grocery\0" "group\0"
    "grow\0" "grunt\0" "guard\0" "guess\0" "guide\0" "guilt\0" "guitar\0" "gun",
    "gym\0" "habit\0" "hair\0" "half\0" "hammer\0" "hamster\0" "hand\0" "happy\0"
    "harbor\0" "hard\0" "harsh\0" "harvest\0" "hat\0" "have\0" "hawk\0" "hazard",
    "head\0" "health\0" "heart\0" "heavy\0" "hedgehog\0" "height\0" "hello\0" "helmet\0"
    "help\0" "hen\0" "hero\0" "hidden\0" "high\0" "hill\0" "hint\0" "hip",
    "hire\0" "history\0" "hobby\0" "hockey\0" "hold\0" "hole\0" "holiday\0" "hollow\0"
    "home\0" "honey\0" "hood\0" "hope\0" "horn\0" "horror\0" "horse\0" "hospital",
    "host\0" "hotel\0" "hour\0" "hover\0" "hub\0" "huge\0" "human\0" "humble\0"
    "humor\0" "hundred\0" "hungry\0" "hunt\0" "hurdle\0" "hurry\0" "hurt\0" "husband",
    "hybrid\0" "ice\0" "icon\0" "idea\0" "identify\0" "idle\0" "ignore\0" "ill\0"
    "illegal\0" "illness\0" "image\0" "imitate\0" "immense\0" "immune\0" "impact\0" "impose",
    "improve\0" "impulse\0" "inch\0" "include\0" "income\0" "increase\0" "index\0" "indicate\0"
    "indoor\0" "industry\0" "infant\0" "inflict\0" "inform\0" "inhale\0" "inherit\0" "initial",
    "inject\0" "injury\0" "inmate\0" "inner\0" "innocent\0" "input\0" "inquiry\0" "insane\0"
    "insect\0" "inside\0" "inspire\0" "install\0" "intact\0" "interest\0" "into\0" "invest",
    "invite\0" "involve\0" "iron\0" "island\0" "isolate\0" "issue\0" "item\0" "ivory\0"
    "jacket\0" "jaguar\0" "jar\0" "jazz\0" "jealous\0" "jeans\0" "jelly\0" "jewel",
    "job\0" "join\0" "joke\0" "journey\0" "joy\0" "judge\0" "juice\0" "jump\0"
    "jungle\0" "junior\0" "junk\0" "just\0" "kangaroo\0" "keen\0" "keep\0" "ketchup",
    "key\0" "kick\0" "kid\0" "kidney\0" "kind\0" "kingdom\0" "kiss\0" "kit\0"
    "kitchen\0" "kite\0" "kitten\0" "kiwi\0" "knee\0" "knife\0" "knock\0" "know",
    "lab\0" "label\0" "labor\0" "ladder\0" "lady\0" "lake\0" "lamp\0" "language\0"
    "laptop\0" "large\0" "later\0" "latin\0" "laugh\0" "laundry\0" "lava\0" "law",
    "lawn\0" "lawsuit\0" "layer\0" "lazy\0" "leader\0" "leaf\0" "learn\0" "leave\0"
    "lecture\0" "left\0" "leg\0" "legal\0" "legend\0" "leisure\0" "lemon\0" "lend",
    "length\0" "lens\0" "leopard\0" "lesson\0" "letter\0" "level\0" "liar\0" "liberty\0"
    "library\0" "license\0" "life\0" "lift\0" "light\0" "like\0" "limb\0" "limit",
    "link\0" "lion\0" "liquid\0" "list\0" "little\0" "live\0" "lizard\0" "load\0"
    "loan\0" "lobster\0" "local\0" "lock\0" "logic\0" "lonely\0" "long\0" "loop",
    "lottery\0" "loud\0" "lounge\0" "love\0" "loyal\0" "lucky\0" "luggage\0" "lumber\0"
    "lunar\0" "lunch\0" "luxury\0" "lyrics\0" "machine\0" "mad\0" "magic\0" "magnet",
    "maid\0" "mail\0" "main\0" "major\0" "make\0" "mammal\0" "man\0" "manage\0"
    "mandate\0" "mango\0" "mansion\0" "manual\0" "maple\0" "marble\0" "march\0" "margin",
    "marine\0" "market\0" "marriage\0" "mask\0" "mass\0" "master\0" "match\0" "material\0"
    "math\0" "matrix\0" "matter\0" "maximum\0" "maze\0" "meadow\0" "mean\0" "measure",
    "meat\0" "mechanic\0" "medal\0" "media\0" "melody\0" "melt\0" "member\0" "memory\0"
    "mention\0" "menu\0" "mercy\0" "merge\0" "merit\0" "merry\0" "mesh\0" "message",
    "metal\0" "method\0" "middle\0" "midnight\0" "milk\0" "million\0" "mimic\0" "mind\0"
    "minimum\0" "minor\0" "minute\0" "miracle\0" "mirror\0" "misery\0" "miss\0" "mistake",
    "mix\0" "mixed\0" "mixture\0" "mobile\0" "model\0" "modify\0" "mom\0" "moment\0"
    "monitor\0" "monkey\0" "monster\0" "month\0" "moon\0" "moral\0" "more\0" "morning",
    "mosquito\0" "mother\0" "motion\0" "motor\0" "mountain\0" "mouse\0" "move\0" "movie\0"
    "much\0" "muffin\0" "mule\0" "multiply\0" "muscle\0" "museum\0" "mushroom\0" "music",
    "must\0" "mutual\0" "myself\0" "mystery\0" "myth\0" "naive\0" "name\0" "napkin\0"
    "narrow\0" "nasty\0" "nation\0" "nature\0" "near\0" "neck\0" "need\0" "negative",
    "neglect\0" "neither\0" "nephew\0" "nerve\0" "nest\0" "net\0" "network\0" "neutral\0"
    "never\0" "news\0" "next\0" "nice\0" "night\0" "noble\0" "noise\0" "nominee",
    "noodle\0" "normal\0" "north\0" "nose\0" "notable\0" "note\0" "nothing\0" "notice\0"
    "novel\0" "now\0" "nuclear\0" "number\0" "nurse\0" "nut\0" "oak\0" "obey",
    "object\0" "oblige\0" "obscure\0" "observe\0" "obtain\0" "obvious\0" "occur\0" "ocean\0"
    "october\0" "odor\0" "off\0" "offer\0" "office\0" "often\0" "oil\0" "okay",
    "old\0" "olive\0" "olympic\0" "omit\0" "once\0" "one\0" "onion\0" "online\0"
    "only\0" "open\0" "opera\0" "opinion\0" "oppose\0" "option\0" "orange\0" "orbit",
    "orchard\0" "order\0" "ordinary\0" "organ\0" "orient\0" "original\0" "orphan\0" "ostrich\0"
    "other\0" "outdoor\0" "outer\0" "output\0" "outside\0" "oval\0" "oven\0" "over",
    "own\0" "owner\0" "oxygen\0" "oyster\0" "ozone\0" "pact\0" "paddle\0" "page\0"
    "pair\0" "palace\0" "palm\0" "panda\0" "panel\0" "panic\0" "panther\0" "paper",
    "parade\0" "parent\0" "park\0" "parrot\0" "party\0" "pass\0" "patch\0" "path\0"
    "patient\0" "patrol\0" "pattern\0" "pause\0" "pave\0" "payment\0" "peace\0" "peanut",
    "pear\0" "peasant\0" "pelican\0" "pen\0" "penalty\0" "pencil\0" "people\0" "pepper\0"
    "perfect\0" "permit\0" "person\0" "pet\0" "phone\0" "photo\0" "phrase\0" "physical",
    "piano\0" "picnic\0" "picture\0" "piece\0" "pig\0" "pigeon\0" "pill\0" "pilot\0"
    "pink\0" "pioneer\0" "pipe\0" "pistol\0" "pitch\0" "pizza\0" "place\0" "planet",
    "plastic\0" "plate\0" "play\0" "please\0" "pledge\0" "pluck\0" "plug\0" "plunge\0"
    "poem\0" "poet\0" "point\0" "polar\0" "pole\0" "police\0" "pond\0" "pony",
    "pool\0" "popular\0" "portion\0" "position\0" "possible\0" "post\0" "potato\0" "pottery\0"
    "poverty\0" "powder\0" "power\0" "practice\0" "praise\0" "predict\0" "prefer\0" "prepare",
    "present\0" "pretty\0" "prevent\0" "price\0" "pride\0" "primary\0" "print\0" "priority\0"
    "prison\0" "private\0" "prize\0" "problem\0" "process\0" "produce\0" "profit\0" "program",
    "project\0" "promote\0" "proof\0" "property\0" "prosper\0" "protect\0" "proud\0" "provide\0"
    "public\0" "pudding\0" "pull\0" "pulp\0" "pulse\0" "pumpkin\0" "punch\0" "pupil",
    "puppy\0" "purchase\0" "purity\0" "purpose\0" "purse\0" "push\0" "put\0" "puzzle\0"
    "pyramid\0" "quality\0" "quantum\0" "quarter\0" "question\0" "quick\0" "quit\0" "quiz",
    "quote\0" "rabbit\0" "raccoon\0" "race\0" "rack\0" "radar\0" "radio\0" "rail\0"
    "rain\0" "raise\0" "rally\0" "ramp\0" "ranch\0" "random\0" "range\0" "rapid",
    "rare\0" "rate\0" "rather\0" "raven\0" "raw\0" "razor\0" "ready\0" "real\0"
    "reason\0" "rebel\0" "rebuild\0" "recall\0" "receive\0" "recipe\0" "record\0" "recycle",
    "reduce\0" "reflect\0" "reform\0" "refuse\0" "region\0" "regret\0" "regular\0" "reject\0"
    "relax\0" "release\0" "relief\0" "rely\0" "remain\0" "remember\0" "remind\0" "remove",
    "render\0" "renew\0" "rent\0" "reopen\0" "repair\0" "repeat\0" "replace\0" "report\0"
    "require\0" "rescue\0" "resemble\0" "resist\0" "resource\0" "response\0" "result\0" "retire",
    "retreat\0" "return\0" "reunion\0" "reveal\0" "review\0" "reward\0" "rhythm\0" "rib\0"
    "ribbon\0" "rice\0" "rich\0" "ride\0" "ridge\0" "rifle\0" "right\0" "rigid",
    "ring\0" "riot\0" "ripple\0" "risk\0" "ritual\0" "rival\0" "river\0" "road\0"
    "roast\0" "robot\0" "robust\0" "rocket\0" "romance\0" "roof\0" "rookie\0" "room",
    "rose\0" "rotate\0" "rough\0" "round\0" "route\0" "royal\0" "rubber\0" "rude\0"
    "rug\0" "rule\0" "run\0" "runway\0" "rural\0" "sad\0" "saddle\0" "sadness",
    "safe\0" "sail\0" "salad\0" "salmon\0" "salon\0" "salt\0" "salute\0" "same\0"
    "sample\0" "sand\0" "satisfy\0" "satoshi\0" "sauce\0" "sausage\0" "save\0" "say",
    "scale\0" "scan\0" "scare\0" "scatter\0" "scene\0" "scheme\0" "school\0" "science\0"
    "scissors\0" "scorpion\0" "scout\0" "scrap\0" "screen\0" "script\0" "scrub\0" "sea",
    "search\0" "season\0" "seat\0" "second\0" "secret\0" "section\0" "security\0" "seed\0"
    "seek\0" "segment\0" "select\0" "sell\0" "seminar\0" "senior\0" "sense\0" "sentence",
    "series\0" "service\0" "session\0" "settle\0" "setup\0" "seven\0" "shadow\0" "shaft\0"
    "shallow\0" "share\0" "shed\0" "shell\0" "sheriff\0" "shield\0" "shift\0" "shine",
    "ship\0" "shiver\0" "shock\0" "shoe\0" "shoot\0" "shop\0" "short\0" "shoulder\0"
    "shove\0" "shrimp\0" "shrug\0" "shuffle\0" "shy\0" "sibling\0" "sick\0" "side",
    "siege\0" "sight\0" "sign\0" "silent\0" "silk\0" "silly\0" "silver\0" "similar\0"
    "simple\0" "since\0" "sing\0" "siren\0" "sister\0" "situate\0" "six\0" "size",
    "skate\0" "sketch\0" "ski\0" "skill\0" "skin\0" "skirt\0" "skull\0" "slab\0"
    "slam\0" "sleep\0" "slender\0" "slice\0" "slide\0" "slight\0" "slim\0" "slogan",
    "slot\0" "slow\0" "slush\0" "small\0" "smart\0" "smile\0" "smoke\0" "smooth\0"
    "snack\0" "snake\0" "snap\0" "sniff\0" "snow\0" "soap\0" "soccer\0" "social",
    "sock\0" "soda\0" "soft\0" "solar\0" "soldier\0" "solid\0" "solution\0" "solve\0"
    "someone\0" "song\0" "soon\0" "sorry\0" "sort\0" "soul\0" "sound\0" "soup",
    "source\0" "south\0" "space\0" "spare\0" "spatial\0" "spawn\0" "speak\0" "special\0"
    "speed\0" "spell\0" "spend\0" "sphere\0" "spice\0" "spider\0" "spike\0" "spin",
    "spirit\0" "split\0" "spoil\0" "sponsor\0" "spoon\0" "sport\0" "spot\0" "spray\0"
    "spread\0" "spring\0" "spy\0" "square\0" "squeeze\0" "squirrel\0" "stable\0" "stadium",
    "staff\0" "stage\0" "stairs\0" "stamp\0" "stand\0" "start\0" "state\0" "stay\0"
    "steak\0" "steel\0" "stem\0" "step\0" "stereo\0" "stick\0" "still\0" "sting",
    "stock\0" "stomach\0" "stone\0" "stool\0" "story\0" "stove\0" "strategy\0" "street\0"
    "strike\0" "strong\0" "struggle\0" "student\0" "stuff\0" "stumble\0" "style\0" "subject",
    "submit\0" "subway\0" "success\0" "such\0" "sudden\0" "suffer\0" "sugar\0" "suggest\0"
    "suit\0" "summer\0" "sun\0" "sunny\0" "sunset\0" "super\0" "supply\0" "supreme",
    "sure\0" "surface\0" "surge\0" "surprise\0" "surround\0" "survey\0" "suspect\0" "sustain\0"
    "swallow\0" "swamp\0" "swap\0" "swarm\0" "swear\0" "sweet\0" "swift\0" "swim",
    "swing\0" "switch\0" "sword\0" "symbol\0" "symptom\0" "syrup\0" "system\0" "table\0"
    "tackle\0" "tag\0" "tail\0" "talent\0" "talk\0" "tank\0" "tape\0" "target",
    "task\0" "taste\0" "tattoo\0" "taxi\0" "teach\0" "team\0" "tell\0" "ten\0"
    "tenant\0" "tennis\0" "tent\0" "term\0" "test\0" "text\0" "thank\0" "that",
    "theme\0" "then\0" "theory\0" "there\0" "they\0" "thing\0" "this\0" "thought\0"
    "three\0" "thrive\0" "throw\0" "thumb\0" "thunder\0" "ticket\0" "tide\0" "tiger",
    "tilt\0" "timber\0" "time\0" "tiny\0" "tip\0" "tired\0" "tissue\0" "title\0"
    "toast\0" "tobacco\0" "today\0" "toddler\0" "toe\0" "together\0" "toilet\0" "token",
    "tomato\0" "tomorrow\0" "tone\0" "tongue\0" "tonight\0" "tool\0" "tooth\0" "top\0"
    "topic\0" "topple\0" "torch\0" "tornado\0" "tortoise\0" "toss\0" "total\0" "tourist",
    "toward\0" "tower\0" "town\0" "toy\0" "track\0" "trade\0" "traffic\0" "tragic\0"
    "train\0" "transfer\0" "trap\0" "trash\0" "travel\0" "tray\0" "treat\0" "tree",
    "trend\0" "trial\0" "tribe\0" "trick\0" "trigger\0" "trim\0" "trip\0" "trophy\0"
    "trouble\0" "truck\0" "true\0" "truly\0" "trumpet\0" "trust\0" "truth\0" "try",
    "tube\0" "tuition\0" "tumble\0" "tuna\0" "tunnel\0" "turkey\0" "turn\0" "turtle\0"
    "twelve\0" "twenty\0" "twice\0" "twin\0" "twist\0" "two\0" "type\0" "typical",
    "ugly\0" "umbrella\0" "unable\0" "unaware\0" "uncle\0" "uncover\0" "under\0" "undo\0"
    "unfair\0" "unfold\0" "unhappy\0" "uniform\0" "unique\0" "unit\0" "universe\0" "unknown",
    "unlock\0" "until\0" "unusual\0" "unveil\0" "update\0" "upgrade\0" "uphold\0" "upon\0"
    "upper\0" "upset\0" "urban\0" "urge\0" "usage\0" "use\0" "used\0" "useful",
    "useless\0" "usual\0" "utility\0" "vacant\0" "vacuum\0" "vague\0" "valid\0" "valley\0"
    "valve\0" "van\0" "vanish\0" "vapor\0" "various\0" "vast\0" "vault\0" "vehicle",
    "velvet\0" "vendor\0" "venture\0" "venue\0" "verb\0" "verify\0" "version\0" "very\0"
    "vessel\0" "veteran\0" "viable\0" "vibrant\0" "vicious\0" "victory\0" "video\0" "view",
    "village\0" "vintage\0" "violin\0" "virtual\0" "virus\0" "visa\0" "visit\0" "visual\0"
    "vital\0" "vivid\0" "vocal\0" "voice\0" "void\0" "volcano\0" "volume\0" "vote",
    "voyage\0" "wage\0" "wagon\0" "wait\0" "walk\0" "wall\0" "walnut\0" "want\0"
    "warfare\0" "warm\0" "warrior\0" "wash\0" "wasp\0" "waste\0" "water\0" "wave",
    "way\0" "wealth\0" "weapon\0" "wear\0" "weasel\0" "weather\0" "web\0" "wedding\0"
    "weekend\0" "weird\0" "welcome\0" "west\0" "wet\0" "whale\0" "what\0" "wheat",
    "wheel\0" "when\0" "where\0" "whip\0" "whisper\0" "wide\0" "width\0" "wife\0"
    "wild\0" "will\0" "win\0" "window\0" "wine\0" "wing\0" "wink\0" "winner",
    "winter\0" "wire\0" "wisdom\0" "wise\0" "wish\0" "witness\0" "wolf\0" "woman\0"
    "wonder\0" "wood\0" "wool\0" "word\0" "work\0" "world\0" "worry\0" "worth",
    "wrap\0" "wreck\0" "wrestle\0" "wrist\0" "write\0" "wrong\0" "yard\0" "year\0"
    "yellow\0" "you\0" "young\0" "youth\0" "zebra\0" "zero\0" "zone\0" "zoo",
};

_Static_assert(sizeof(WORD_GROUPS) / sizeof(*WORD_GROUPS) * WORD_GROUP_LENGTH == SC2_WORDLIST_SIZE,
               "incorrect number of word groups");

const char *sc2_wordlist_en(uint16_t idx)
{
    if (idx >= SC2_WORDLIST_SIZE) {
        return NULL;
    }
    const char *word = WORD_GROUPS[idx / WORD_GROUP_LENGTH];
    for (unsigned i = idx % WORD_GROUP_LENGTH; i > 0; --i) {
        word += strlen(word) + 1U;
    }
    return word;
}
