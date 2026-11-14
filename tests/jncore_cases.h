// Made by gen_jncore_cases.py from GNU join's answers. Do not edit.
typedef struct { const char* s; int ok; unsigned long long val; } jn_field_case_t;
typedef struct { const char* s; int which; } jn_filenum_case_t;
// status: 0 good, 1 invalid field specifier, 2 invalid file number, 3 invalid field number; field -1: past the end, not seen.
typedef struct { const char* s; int status, file; int field; } jn_spec_case_t;
typedef struct { const char* s; unsigned n; } jn_str_t;
typedef struct { int tab; jn_str_t line; unsigned nf; const jn_str_t* f; } jn_cut_case_t;
typedef struct { jn_str_t a, b; int icase, order; } jn_key_case_t;

static const jn_field_case_t jn_field_cases[] = {
    {"1", 1, 0ull},
    {"2", 1, 1ull},
    {"3", 1, 2ull},
    {"7", 1, 6ull},
    {"10", 1, 9ull},
    {"40", 1, 39ull},
    {"41", 1, 40ull},
    {"0", 0, 0ull},
    {"-1", 0, 0ull},
    {"+1", 1, 0ull},
    {"+0", 0, 0ull},
    {"-0", 0, 0ull},
    {" 1", 1, 0ull},
    {"\0112", 1, 1ull},
    {"\0123", 1, 2ull},
    {"\0134", 1, 3ull},
    {"\0145", 1, 4ull},
    {"\0156", 1, 5ull},
    {"  7", 1, 6ull},
    {"1 ", 0, 0ull},
    {" 1 ", 0, 0ull},
    {"", 0, 0ull},
    {"+", 0, 0ull},
    {"-", 0, 0ull},
    {"1x", 0, 0ull},
    {"x1", 0, 0ull},
    {"0x10", 0, 0ull},
    {"010", 1, 9ull},
    {"00", 0, 0ull},
    {"1e3", 0, 0ull},
    {"1.0", 0, 0ull},
    {"1,2", 0, 0ull},
    {"99999999999999999999", 1, 9223372036854775806ull},
    {"9223372036854775807", 1, 9223372036854775806ull},
    {"9223372036854775808", 1, 9223372036854775806ull},
    {"9223372036854775806", 1, 9223372036854775805ull},
    {"9223372036854775805", 1, 9223372036854775804ull},
    {"18446744073709551616", 1, 9223372036854775806ull},
    {"-9223372036854775808", 0, 0ull},
    {"-9223372036854775809", 1, 9223372036854775806ull},
    {"-99999999999999999999", 1, 9223372036854775806ull},
    {"4294967295", 1, 4294967294ull},
    {"4294967296", 1, 4294967295ull},
    {"2147483647", 1, 2147483646ull},
    {"2147483648", 1, 2147483647ull},
    {"1_0", 0, 0ull},
    {"+ 1", 0, 0ull},
    {"- 1", 0, 0ull},
    {"++1", 0, 0ull},
    {"--1", 0, 0ull},
    {"+-1", 0, 0ull},
    {"0 ", 0, 0ull},
    {"99999999999999999999x", 0, 0ull},
    {"99999999999999999999 ", 0, 0ull},
    {"00000000000000000000000000000001", 1, 0ull},
    {"000000000000000000000000000000099999999999999999999", 1, 9223372036854775806ull},
    {"\2401", 0, 0ull},
    {"\2051", 0, 0ull},
    {"1\013", 0, 0ull},
    {"1\012", 0, 0ull},
    {"\012\0121", 1, 0ull},
    {" +5", 1, 4ull},
    {"+ 5", 0, 0ull},
    {"9223372036854775807x", 0, 0ull},
    {"0000009223372036854775807", 1, 9223372036854775806ull},
    {"9223372036854775808x", 0, 0ull},
};

static const jn_filenum_case_t jn_filenum_cases[] = {
    {"1", 1},
    {"2", 2},
    {"3", 0},
    {"0", 0},
    {"-1", 0},
    {"+1", 1},
    {"+2", 2},
    {" 1", 1},
    {"1 ", 0},
    {"01", 1},
    {"002", 2},
    {"2x", 0},
    {"", 0},
    {"x", 0},
    {"1.0", 0},
    {"99999999999999999999", 0},
    {"9223372036854775809", 0},
    {"-2", 0},
    {"\0112", 2},
    {"\0121", 1},
    {"+", 0},
    {" ", 0},
    {"1\012", 0},
    {"0x1", 0},
    {"1e0", 0},
    {"4294967297", 0},
    {"18446744073709551617", 0},
    {"-18446744073709551615", 0},
};

static const jn_spec_case_t jn_spec_cases[] = {
    {"0", 0, 0, 0},
    {"1.1", 0, 1, 0},
    {"1.2", 0, 1, 1},
    {"2.7", 0, 2, 6},
    {"2.60", 0, 2, 59},
    {"1.41", 0, 1, 40},
    {"2.59", 0, 2, 58},
    {"1.61", 0, 1, -1},
    {"0.1", 1, 0, 0},
    {"0.", 1, 0, 0},
    {"00", 1, 0, 0},
    {"1", 1, 0, 0},
    {"2", 1, 0, 0},
    {"1.", 3, 0, 0},
    {"2.", 3, 0, 0},
    {"1.0", 3, 0, 0},
    {"1.-1", 3, 0, 0},
    {"1.+3", 0, 1, 2},
    {"2.007", 0, 2, 6},
    {"1.x", 3, 0, 0},
    {"3.1", 2, 0, 0},
    {"3", 2, 0, 0},
    {"a", 2, 0, 0},
    {"x.1", 2, 0, 0},
    {"", 2, 0, 0},
    {"1:1", 1, 0, 0},
    {"1.1.1", 3, 0, 0},
    {"1.99999999999999999999", 0, 1, -1},
    {"2.9223372036854775807", 0, 2, -1},
    {"1.9223372036854775808", 0, 1, -1},
    {"1.\0133", 0, 1, 2},
    {"2.\0124", 0, 2, 3},
    {"1.3x", 3, 0, 0},
    {"9.1", 2, 0, 0},
    {"01.1", 1, 0, 0},
    {"1.01", 0, 1, 0},
    {"1.+0", 3, 0, 0},
    {"-1.1", 2, 0, 0},
    {"+1.1", 2, 0, 0},
    {"1.1\001", 3, 0, 0},
    {"0\001", 1, 0, 0},
    {"1.60", 0, 1, 59},
    {"2.1", 0, 2, 0},
};

static const jn_str_t jn_cut_5[] = {{"a", 1}};
static const jn_str_t jn_cut_6[] = {{"a", 1}};
static const jn_str_t jn_cut_7[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_8[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_9[] = {{"a", 1}, {"b", 1}};
static const jn_str_t jn_cut_10[] = {{"a", 1}, {"b", 1}};
static const jn_str_t jn_cut_11[] = {{"a", 1}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_12[] = {{"a", 1}, {"b", 1}};
static const jn_str_t jn_cut_13[] = {{"a", 1}, {"b", 1}};
static const jn_str_t jn_cut_14[] = {{"a", 1}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_15[] = {{"a", 1}, {"b", 1}, {"c", 1}, {"d", 1}, {"e", 1}, {"f", 1}, {"g", 1}, {"h", 1}};
static const jn_str_t jn_cut_16[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_17[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_18[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_19[] = {{"x", 1}, {"y", 1}, {"z", 1}, {"w", 1}};
static const jn_str_t jn_cut_20[] = {{"x", 1}};
static const jn_str_t jn_cut_21[] = {{"abc", 3}};
static const jn_str_t jn_cut_22[] = {{"ab", 2}, {"cd", 2}, {"ef", 2}, {"gh", 2}, {"", 0}};
static const jn_str_t jn_cut_24[] = {{"b", 1}, {"xy", 2}};
static const jn_str_t jn_cut_26[] = {{"a", 1}, {"aa", 2}};
static const jn_str_t jn_cut_27[] = {{"aa", 2}, {"xya", 3}, {"bb", 2}};
static const jn_str_t jn_cut_28[] = {{"aaa", 3}};
static const jn_str_t jn_cut_29[] = {{"b", 1}, {"a", 1}};
static const jn_str_t jn_cut_32[] = {{"ba", 2}};
static const jn_str_t jn_cut_33[] = {{"ba", 2}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_34[] = {{"xya", 3}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_35[] = {{"xyb", 3}, {"", 0}};
static const jn_str_t jn_cut_37[] = {{"ab", 2}, {"aabb", 4}};
static const jn_str_t jn_cut_38[] = {{"xy", 2}, {"bxy", 3}};
static const jn_str_t jn_cut_39[] = {{"a", 1}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_40[] = {{"axy", 3}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_41[] = {{"b", 1}};
static const jn_str_t jn_cut_42[] = {{"b", 1}, {"xya", 3}, {"", 0}};
static const jn_str_t jn_cut_43[] = {{"a", 1}};
static const jn_str_t jn_cut_44[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_45[] = {{"aba", 3}, {"xyb", 3}, {"ab", 2}, {"", 0}};
static const jn_str_t jn_cut_46[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_47[] = {{"a", 1}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_48[] = {{"aaxya", 5}, {"xyxyb", 5}};
static const jn_str_t jn_cut_49[] = {{"axya", 4}};
static const jn_str_t jn_cut_50[] = {{"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_51[] = {{"a", 1}, {"aa", 2}};
static const jn_str_t jn_cut_52[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_53[] = {{"a", 1}, {"bxy", 3}, {"baa", 3}};
static const jn_str_t jn_cut_54[] = {{"a", 1}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_55[] = {{"a", 1}, {"a", 1}, {"xy", 2}};
static const jn_str_t jn_cut_56[] = {{"aa", 2}, {"", 0}};
static const jn_str_t jn_cut_57[] = {{"aa", 2}, {"a", 1}};
static const jn_str_t jn_cut_58[] = {{"aa", 2}, {"aa", 2}, {"", 0}};
static const jn_str_t jn_cut_59[] = {{"aaa", 3}, {"a", 1}, {"aa", 2}, {"", 0}};
static const jn_str_t jn_cut_60[] = {{"aaaa", 4}};
static const jn_str_t jn_cut_61[] = {{"aaaaxy", 6}, {"", 0}};
static const jn_str_t jn_cut_62[] = {{"aba", 3}, {"ba", 2}, {"", 0}};
static const jn_str_t jn_cut_63[] = {{"b", 1}};
static const jn_str_t jn_cut_64[] = {{"b", 1}, {"b", 1}, {"bba", 3}};
static const jn_str_t jn_cut_65[] = {{"b", 1}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_66[] = {{"b", 1}, {"", 0}};
static const jn_str_t jn_cut_67[] = {{"ba", 2}, {"", 0}};
static const jn_str_t jn_cut_68[] = {{"ba", 2}, {"a", 1}, {"a", 1}, {"a", 1}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_69[] = {{"baa", 3}, {"xyba", 4}, {"aa", 2}, {"", 0}};
static const jn_str_t jn_cut_70[] = {{"bb", 2}, {"a", 1}, {"a", 1}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_71[] = {{"bxy", 3}, {"xyxya", 5}, {"b", 1}};
static const jn_str_t jn_cut_72[] = {{"xy", 2}, {"a", 1}, {"a", 1}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_73[] = {{"xy", 2}, {"b", 1}};
static const jn_str_t jn_cut_74[] = {{"xyaa", 4}, {"", 0}};
static const jn_str_t jn_cut_75[] = {{"xyba", 4}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_77[] = {{"a", 1}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_78[] = {{"a", 1}, {"a", 1}};
static const jn_str_t jn_cut_80[] = {{"a", 1}, {"xyxy", 4}};
static const jn_str_t jn_cut_81[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_82[] = {{"aa", 2}, {"aa", 2}, {"", 0}};
static const jn_str_t jn_cut_83[] = {{"b", 1}, {"", 0}};
static const jn_str_t jn_cut_84[] = {{"xy", 2}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_85[] = {{"a", 1}, {"xya", 3}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_86[] = {{"a", 1}, {"abbb", 4}, {"", 0}};
static const jn_str_t jn_cut_87[] = {{"aa", 2}, {"", 0}};
static const jn_str_t jn_cut_88[] = {{"bb", 2}, {"", 0}};
static const jn_str_t jn_cut_89[] = {{"bxy", 3}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_90[] = {{"xy", 2}, {"bbxy", 4}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_91[] = {{"xyaxyb", 6}};
static const jn_str_t jn_cut_92[] = {{"xyb", 3}, {"xybb", 4}, {"aaa", 3}, {"", 0}};
static const jn_str_t jn_cut_93[] = {{"xyxy", 4}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_95[] = {{"ab", 2}, {"b", 1}};
static const jn_str_t jn_cut_96[] = {{"xyaaa", 5}, {"baa", 3}};
static const jn_str_t jn_cut_97[] = {{"xya", 3}, {"a", 1}};
static const jn_str_t jn_cut_98[] = {{"ab", 2}};
static const jn_str_t jn_cut_99[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_100[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_101[] = {{"a", 1}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_102[] = {{"aaaaa", 5}};
static const jn_str_t jn_cut_103[] = {{"b", 1}, {"", 0}};
static const jn_str_t jn_cut_104[] = {{"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_105[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_106[] = {{"aa", 2}, {"", 0}};
static const jn_str_t jn_cut_107[] = {{"ba", 2}, {"bxy", 3}, {"abxy", 4}};
static const jn_str_t jn_cut_108[] = {{"xy", 2}};
static const jn_str_t jn_cut_111[] = {{"axy", 3}, {"xyaaa", 5}, {"b", 1}};
static const jn_str_t jn_cut_113[] = {{"b", 1}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_114[] = {{"b", 1}, {"aaa", 3}, {"", 0}};
static const jn_str_t jn_cut_115[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_116[] = {{"aaxy", 4}, {"aab", 3}};
static const jn_str_t jn_cut_117[] = {{"bb", 2}, {"b", 1}, {"bxy", 3}};
static const jn_str_t jn_cut_118[] = {{"a", 1}};
static const jn_str_t jn_cut_119[] = {{"a", 1}, {"a", 1}, {"aa", 2}, {"", 0}};
static const jn_str_t jn_cut_120[] = {{"a", 1}, {"xy", 2}};
static const jn_str_t jn_cut_121[] = {{"a", 1}, {"axy", 3}};
static const jn_str_t jn_cut_122[] = {{"a", 1}, {"bxy", 3}, {"", 0}};
static const jn_str_t jn_cut_123[] = {{"aa", 2}, {"", 0}};
static const jn_str_t jn_cut_124[] = {{"axy", 3}};
static const jn_str_t jn_cut_125[] = {{"b", 1}, {"b", 1}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_126[] = {{"xy", 2}, {"aaxya", 5}};
static const jn_str_t jn_cut_127[] = {{"a", 1}, {"xy", 2}, {"a", 1}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_128[] = {{"aaa", 3}, {"", 0}};
static const jn_str_t jn_cut_129[] = {{"aba", 3}, {"b", 1}, {"a", 1}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_130[] = {{"abaab", 5}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_131[] = {{"axyxy", 5}};
static const jn_str_t jn_cut_132[] = {{"b", 1}, {"bxy", 3}};
static const jn_str_t jn_cut_133[] = {{"xy", 2}, {"a", 1}, {"bb", 2}};
static const jn_str_t jn_cut_134[] = {{"xyaa", 4}, {"aaxya", 5}, {"", 0}};
static const jn_str_t jn_cut_135[] = {{"xybaabxya", 9}, {"a", 1}, {"ba", 2}};
static const jn_str_t jn_cut_136[] = {{"a", 1}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_137[] = {{"a", 1}, {"axya", 4}, {"", 0}};
static const jn_str_t jn_cut_138[] = {{"aa", 2}, {"aaxy", 4}};
static const jn_str_t jn_cut_139[] = {{"aaaabxy", 7}, {"a", 1}, {"b", 1}};
static const jn_str_t jn_cut_140[] = {{"axyab", 5}, {"aa", 2}};
static const jn_str_t jn_cut_141[] = {{"axyaxy", 6}};
static const jn_str_t jn_cut_142[] = {{"b", 1}, {"aba", 3}};
static const jn_str_t jn_cut_143[] = {{"b", 1}, {"a", 1}};
static const jn_str_t jn_cut_144[] = {{"xy", 2}};
static const jn_str_t jn_cut_145[] = {{"xy", 2}, {"bba", 3}, {"", 0}};
static const jn_str_t jn_cut_146[] = {{"xy", 2}, {"xyab", 4}, {"xy", 2}};
static const jn_str_t jn_cut_147[] = {{"xyaaa", 5}, {"", 0}};
static const jn_str_t jn_cut_148[] = {{"a", 1}};
static const jn_str_t jn_cut_149[] = {{"a", 1}, {"bbaaa", 5}};
static const jn_str_t jn_cut_150[] = {{"a", 1}, {"ab", 2}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_151[] = {{"a", 1}, {"xyxy", 4}, {"xyb", 3}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_152[] = {{"a", 1}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_153[] = {{"a", 1}, {"a", 1}, {"ba", 2}, {"", 0}};
static const jn_str_t jn_cut_154[] = {{"a", 1}, {"xy", 2}, {"xyxya", 5}, {"", 0}};
static const jn_str_t jn_cut_155[] = {{"aa", 2}, {"", 0}};
static const jn_str_t jn_cut_156[] = {{"aa", 2}, {"aab", 3}};
static const jn_str_t jn_cut_157[] = {{"aaaaaba", 7}, {"", 0}};
static const jn_str_t jn_cut_158[] = {{"aaxyaa", 6}, {"aaa", 3}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_159[] = {{"ab", 2}, {"axy", 3}, {"abaa", 4}};
static const jn_str_t jn_cut_160[] = {{"ab", 2}, {"a", 1}, {"xyb", 3}, {"ab", 2}};
static const jn_str_t jn_cut_161[] = {{"aba", 3}, {"", 0}};
static const jn_str_t jn_cut_162[] = {{"aba", 3}, {"xy", 2}, {"xyxy", 4}, {"", 0}};
static const jn_str_t jn_cut_163[] = {{"axy", 3}, {"xya", 3}, {"", 0}};
static const jn_str_t jn_cut_164[] = {{"axyxy", 5}, {"bxy", 3}};
static const jn_str_t jn_cut_165[] = {{"b", 1}, {"", 0}};
static const jn_str_t jn_cut_166[] = {{"b", 1}, {"abaxy", 5}, {"", 0}};
static const jn_str_t jn_cut_167[] = {{"b", 1}, {"b", 1}, {"a", 1}};
static const jn_str_t jn_cut_168[] = {{"b", 1}, {"xy", 2}};
static const jn_str_t jn_cut_169[] = {{"ba", 2}, {"", 0}};
static const jn_str_t jn_cut_170[] = {{"baba", 4}, {"b", 1}, {"a", 1}, {"a", 1}};
static const jn_str_t jn_cut_171[] = {{"bb", 2}, {"a", 1}, {"aaxy", 4}};
static const jn_str_t jn_cut_172[] = {{"xy", 2}, {"xyba", 4}, {"", 0}};
static const jn_str_t jn_cut_173[] = {{"xy", 2}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_174[] = {{"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_175[] = {{"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_176[] = {{"xy", 2}, {"ab", 2}, {"ab", 2}};
static const jn_str_t jn_cut_177[] = {{"xya", 3}, {"baxy", 4}, {"b", 1}};
static const jn_str_t jn_cut_178[] = {{"a", 1}, {"axyxy", 5}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_179[] = {{"a", 1}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_180[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_181[] = {{"a", 1}, {"ba", 2}, {"aaxy", 4}, {"", 0}};
static const jn_str_t jn_cut_182[] = {{"a", 1}, {"xy", 2}};
static const jn_str_t jn_cut_183[] = {{"a", 1}, {"a", 1}, {"xy", 2}, {"a", 1}};
static const jn_str_t jn_cut_184[] = {{"a", 1}, {"a", 1}, {"a", 1}, {"xy", 2}};
static const jn_str_t jn_cut_185[] = {{"aa", 2}, {"b", 1}};
static const jn_str_t jn_cut_186[] = {{"aa", 2}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_187[] = {{"aaa", 3}, {"ab", 2}, {"baa", 3}};
static const jn_str_t jn_cut_188[] = {{"aaa", 3}, {"a", 1}, {"xyb", 3}};
static const jn_str_t jn_cut_189[] = {{"aaaxy", 5}, {"xy", 2}, {"a", 1}, {"b", 1}};
static const jn_str_t jn_cut_190[] = {{"aabb", 4}};
static const jn_str_t jn_cut_191[] = {{"ab", 2}, {"a", 1}, {"aba", 3}, {"a", 1}};
static const jn_str_t jn_cut_192[] = {{"abbaxy", 6}, {"aa", 2}, {"ab", 2}};
static const jn_str_t jn_cut_193[] = {{"axya", 4}, {"aa", 2}, {"xy", 2}, {"a", 1}};
static const jn_str_t jn_cut_194[] = {{"axyaaab", 7}, {"", 0}};
static const jn_str_t jn_cut_195[] = {{"axyab", 5}};
static const jn_str_t jn_cut_196[] = {{"axybxy", 6}, {"xy", 2}, {"bxy", 3}};
static const jn_str_t jn_cut_197[] = {{"axyxya", 6}};
static const jn_str_t jn_cut_198[] = {{"b", 1}, {"a", 1}, {"xya", 3}};
static const jn_str_t jn_cut_199[] = {{"b", 1}, {"", 0}};
static const jn_str_t jn_cut_200[] = {{"b", 1}, {"", 0}};
static const jn_str_t jn_cut_201[] = {{"b", 1}, {"", 0}};
static const jn_str_t jn_cut_202[] = {{"b", 1}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_203[] = {{"b", 1}, {"xyaxy", 5}, {"aba", 3}};
static const jn_str_t jn_cut_204[] = {{"bb", 2}, {"a", 1}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_205[] = {{"bxybaa", 6}, {"abb", 3}};
static const jn_str_t jn_cut_206[] = {{"xy", 2}, {"a", 1}, {"xyaaa", 5}, {"", 0}};
static const jn_str_t jn_cut_207[] = {{"xy", 2}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_208[] = {{"xy", 2}, {"a", 1}, {"b", 1}, {"b", 1}};
static const jn_str_t jn_cut_209[] = {{"xy", 2}, {"aaa", 3}, {"ab", 2}};
static const jn_str_t jn_cut_210[] = {{"xya", 3}, {"a", 1}, {"b", 1}, {"a", 1}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_211[] = {{"xyaa", 4}, {"b", 1}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_212[] = {{"xyb", 3}, {"xyxy", 4}};
static const jn_str_t jn_cut_213[] = {{"xybxyxy", 7}, {"axy", 3}};
static const jn_str_t jn_cut_214[] = {{"xyxy", 4}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_215[] = {{"xyxyb", 5}, {"", 0}};
static const jn_str_t jn_cut_216[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_217[] = {{"a", 1}, {"aa", 2}, {"aa", 2}, {"", 0}};
static const jn_str_t jn_cut_218[] = {{"a", 1}, {"b", 1}, {"axy", 3}, {"ba", 2}};
static const jn_str_t jn_cut_219[] = {{"a", 1}, {"xyaa", 4}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_220[] = {{"a", 1}, {"aa", 2}, {"a", 1}};
static const jn_str_t jn_cut_221[] = {{"a", 1}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_222[] = {{"a", 1}, {"b", 1}};
static const jn_str_t jn_cut_223[] = {{"a", 1}, {"bxya", 4}, {"xy", 2}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_224[] = {{"a", 1}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_225[] = {{"a", 1}, {"a", 1}, {"xya", 3}};
static const jn_str_t jn_cut_226[] = {{"a", 1}, {"aaaa", 4}, {"ba", 2}, {"", 0}};
static const jn_str_t jn_cut_227[] = {{"a", 1}, {"aba", 3}};
static const jn_str_t jn_cut_228[] = {{"a", 1}, {"axy", 3}};
static const jn_str_t jn_cut_229[] = {{"a", 1}, {"bxyxy", 5}, {"", 0}};
static const jn_str_t jn_cut_230[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_231[] = {{"a", 1}, {"xy", 2}};
static const jn_str_t jn_cut_232[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_233[] = {{"a", 1}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_234[] = {{"a", 1}, {"b", 1}};
static const jn_str_t jn_cut_235[] = {{"a", 1}, {"xyb", 3}, {"", 0}};
static const jn_str_t jn_cut_236[] = {{"a", 1}, {"a", 1}, {"axy", 3}, {"", 0}};
static const jn_str_t jn_cut_237[] = {{"a", 1}, {"a", 1}, {"b", 1}};
static const jn_str_t jn_cut_238[] = {{"a", 1}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_239[] = {{"a", 1}, {"xya", 3}};
static const jn_str_t jn_cut_240[] = {{"a", 1}, {"xyxy", 4}, {"bb", 2}, {"", 0}};
static const jn_str_t jn_cut_241[] = {{"a", 1}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_242[] = {{"a", 1}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_243[] = {{"a", 1}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_244[] = {{"a", 1}, {"aa", 2}, {"xyaa", 4}, {"ba", 2}, {"", 0}};
static const jn_str_t jn_cut_245[] = {{"a", 1}, {"bb", 2}, {"", 0}};
static const jn_str_t jn_cut_246[] = {{"a", 1}, {"xy", 2}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_247[] = {{"a", 1}, {"a", 1}};
static const jn_str_t jn_cut_248[] = {{"a", 1}, {"a", 1}, {"a", 1}};
static const jn_str_t jn_cut_249[] = {{"a", 1}, {"a", 1}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_250[] = {{"a", 1}, {"axy", 3}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_251[] = {{"a", 1}, {"b", 1}, {"a", 1}, {"b", 1}, {"aab", 3}};
static const jn_str_t jn_cut_252[] = {{"a", 1}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_253[] = {{"a", 1}, {"b", 1}, {"a", 1}};
static const jn_str_t jn_cut_254[] = {{"a", 1}, {"bb", 2}};
static const jn_str_t jn_cut_255[] = {{"aa", 2}};
static const jn_str_t jn_cut_256[] = {{"aa", 2}, {"", 0}};
static const jn_str_t jn_cut_257[] = {{"aa", 2}, {"a", 1}};
static const jn_str_t jn_cut_258[] = {{"aa", 2}, {"a", 1}};
static const jn_str_t jn_cut_259[] = {{"aa", 2}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_260[] = {{"aa", 2}, {"abaaxybbb", 9}};
static const jn_str_t jn_cut_261[] = {{"aa", 2}, {"aa", 2}, {"xyxy", 4}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_262[] = {{"aa", 2}, {"xyaxy", 5}, {"a", 1}};
static const jn_str_t jn_cut_263[] = {{"aaa", 3}};
static const jn_str_t jn_cut_264[] = {{"aaaa", 4}, {"a", 1}, {"ab", 2}};
static const jn_str_t jn_cut_265[] = {{"aaaaa", 5}, {"baa", 3}, {"a", 1}};
static const jn_str_t jn_cut_266[] = {{"aaaaxyxya", 9}, {"xyaxy", 5}};
static const jn_str_t jn_cut_267[] = {{"aab", 3}, {"", 0}};
static const jn_str_t jn_cut_268[] = {{"aab", 3}, {"", 0}};
static const jn_str_t jn_cut_269[] = {{"aabb", 4}, {"b", 1}};
static const jn_str_t jn_cut_270[] = {{"aaxy", 4}, {"xy", 2}, {"aaxyb", 5}, {"bxy", 3}};
static const jn_str_t jn_cut_271[] = {{"ab", 2}, {"axy", 3}};
static const jn_str_t jn_cut_272[] = {{"ab", 2}, {"axy", 3}, {"axyaxya", 7}, {"", 0}};
static const jn_str_t jn_cut_273[] = {{"ab", 2}, {"a", 1}, {"b", 1}, {"xy", 2}, {"a", 1}};
static const jn_str_t jn_cut_274[] = {{"ab", 2}, {"aa", 2}, {"b", 1}, {"a", 1}, {"a", 1}};
static const jn_str_t jn_cut_275[] = {{"ab", 2}, {"bb", 2}, {"", 0}};
static const jn_str_t jn_cut_276[] = {{"aba", 3}};
static const jn_str_t jn_cut_277[] = {{"aba", 3}, {"bxyab", 5}, {"", 0}};
static const jn_str_t jn_cut_278[] = {{"abaa", 4}, {"baa", 3}, {"", 0}};
static const jn_str_t jn_cut_279[] = {{"abaxya", 6}, {"aaa", 3}, {"", 0}};
static const jn_str_t jn_cut_280[] = {{"abb", 3}, {"a", 1}};
static const jn_str_t jn_cut_281[] = {{"abxy", 4}};
static const jn_str_t jn_cut_282[] = {{"abxy", 4}, {"bab", 3}, {"b", 1}};
static const jn_str_t jn_cut_283[] = {{"abxy", 4}, {"aa", 2}};
static const jn_str_t jn_cut_284[] = {{"axy", 3}, {"a", 1}};
static const jn_str_t jn_cut_285[] = {{"axy", 3}, {"aaxyaaa", 7}, {"", 0}};
static const jn_str_t jn_cut_286[] = {{"axy", 3}, {"aa", 2}, {"a", 1}};
static const jn_str_t jn_cut_287[] = {{"axy", 3}, {"a", 1}};
static const jn_str_t jn_cut_288[] = {{"axy", 3}, {"b", 1}, {"a", 1}, {"a", 1}, {"b", 1}};
static const jn_str_t jn_cut_289[] = {{"axy", 3}, {"xy", 2}, {"b", 1}};
static const jn_str_t jn_cut_290[] = {{"axya", 4}, {"xyxya", 5}, {"", 0}};
static const jn_str_t jn_cut_291[] = {{"axyabaxya", 9}, {"", 0}};
static const jn_str_t jn_cut_292[] = {{"axyaxyaba", 9}, {"a", 1}};
static const jn_str_t jn_cut_293[] = {{"axyb", 4}, {"a", 1}, {"a", 1}};
static const jn_str_t jn_cut_294[] = {{"axyb", 4}, {"a", 1}};
static const jn_str_t jn_cut_295[] = {{"axyba", 5}, {"a", 1}, {"aaxy", 4}, {"", 0}};
static const jn_str_t jn_cut_296[] = {{"axyxy", 5}};
static const jn_str_t jn_cut_297[] = {{"axyxy", 5}, {"baa", 3}, {"", 0}};
static const jn_str_t jn_cut_298[] = {{"axyxy", 5}, {"bab", 3}, {"b", 1}};
static const jn_str_t jn_cut_299[] = {{"axyxya", 6}, {"a", 1}};
static const jn_str_t jn_cut_300[] = {{"b", 1}};
static const jn_str_t jn_cut_301[] = {{"b", 1}, {"a", 1}, {"abbxy", 5}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_302[] = {{"b", 1}, {"", 0}};
static const jn_str_t jn_cut_303[] = {{"b", 1}, {"axyaba", 6}};
static const jn_str_t jn_cut_304[] = {{"b", 1}, {"a", 1}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_305[] = {{"b", 1}, {"baa", 3}, {"", 0}};
static const jn_str_t jn_cut_306[] = {{"b", 1}, {"baab", 4}};
static const jn_str_t jn_cut_307[] = {{"b", 1}, {"baxya", 5}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_308[] = {{"b", 1}, {"", 0}};
static const jn_str_t jn_cut_309[] = {{"b", 1}, {"a", 1}};
static const jn_str_t jn_cut_310[] = {{"b", 1}, {"xyb", 3}, {"b", 1}};
static const jn_str_t jn_cut_311[] = {{"b", 1}, {"a", 1}, {"b", 1}};
static const jn_str_t jn_cut_312[] = {{"b", 1}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_313[] = {{"b", 1}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_314[] = {{"b", 1}, {"b", 1}, {"b", 1}, {"xyxya", 5}, {"a", 1}};
static const jn_str_t jn_cut_315[] = {{"b", 1}, {"ba", 2}, {"axya", 4}, {"xy", 2}};
static const jn_str_t jn_cut_316[] = {{"b", 1}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_317[] = {{"b", 1}, {"xy", 2}, {"bxy", 3}, {"", 0}};
static const jn_str_t jn_cut_318[] = {{"b", 1}, {"a", 1}, {"a", 1}, {"b", 1}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_319[] = {{"b", 1}, {"babb", 4}, {"", 0}};
static const jn_str_t jn_cut_320[] = {{"b", 1}, {"bb", 2}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_321[] = {{"b", 1}, {"a", 1}, {"a", 1}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_322[] = {{"b", 1}, {"a", 1}, {"b", 1}, {"bbba", 4}, {"", 0}};
static const jn_str_t jn_cut_323[] = {{"b", 1}, {"ab", 2}};
static const jn_str_t jn_cut_324[] = {{"b", 1}, {"ab", 2}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_325[] = {{"b", 1}, {"aba", 3}, {"xya", 3}};
static const jn_str_t jn_cut_326[] = {{"b", 1}, {"axya", 4}, {"a", 1}, {"b", 1}, {"a", 1}};
static const jn_str_t jn_cut_327[] = {{"b", 1}, {"b", 1}, {"a", 1}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_328[] = {{"b", 1}, {"bb", 2}, {"a", 1}, {"aaaba", 5}};
static const jn_str_t jn_cut_329[] = {{"b", 1}, {"xy", 2}};
static const jn_str_t jn_cut_330[] = {{"b", 1}, {"xy", 2}, {"ba", 2}, {"", 0}};
static const jn_str_t jn_cut_331[] = {{"b", 1}, {"xyaa", 4}, {"axyaa", 5}, {"", 0}};
static const jn_str_t jn_cut_332[] = {{"b", 1}, {"xyxy", 4}, {"bxy", 3}, {"xy", 2}};
static const jn_str_t jn_cut_333[] = {{"ba", 2}, {"a", 1}, {"a", 1}, {"b", 1}, {"a", 1}, {"a", 1}};
static const jn_str_t jn_cut_334[] = {{"ba", 2}, {"", 0}};
static const jn_str_t jn_cut_335[] = {{"ba", 2}, {"", 0}};
static const jn_str_t jn_cut_336[] = {{"ba", 2}, {"xyb", 3}};
static const jn_str_t jn_cut_337[] = {{"ba", 2}, {"b", 1}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_338[] = {{"baa", 3}, {"bxy", 3}, {"", 0}};
static const jn_str_t jn_cut_339[] = {{"bab", 3}};
static const jn_str_t jn_cut_340[] = {{"baxy", 4}};
static const jn_str_t jn_cut_341[] = {{"baxy", 4}, {"ba", 2}, {"", 0}};
static const jn_str_t jn_cut_342[] = {{"bb", 2}, {"", 0}};
static const jn_str_t jn_cut_343[] = {{"bb", 2}, {"a", 1}};
static const jn_str_t jn_cut_344[] = {{"bba", 3}, {"", 0}};
static const jn_str_t jn_cut_345[] = {{"bxy", 3}};
static const jn_str_t jn_cut_346[] = {{"bxy", 3}, {"aaa", 3}, {"aa", 2}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_347[] = {{"bxy", 3}, {"xy", 2}, {"aa", 2}};
static const jn_str_t jn_cut_348[] = {{"bxy", 3}, {"b", 1}, {"a", 1}};
static const jn_str_t jn_cut_349[] = {{"bxya", 4}, {"aaa", 3}};
static const jn_str_t jn_cut_350[] = {{"bxyb", 4}, {"a", 1}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_351[] = {{"bxyxy", 5}};
static const jn_str_t jn_cut_352[] = {{"bxyxybaab", 9}};
static const jn_str_t jn_cut_353[] = {{"xy", 2}};
static const jn_str_t jn_cut_354[] = {{"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_355[] = {{"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_356[] = {{"xy", 2}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_357[] = {{"xy", 2}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_358[] = {{"xy", 2}, {"axy", 3}, {"a", 1}, {"aaa", 3}};
static const jn_str_t jn_cut_359[] = {{"xy", 2}, {"xy", 2}};
static const jn_str_t jn_cut_360[] = {{"xy", 2}, {"xyxy", 4}, {"aa", 2}, {"b", 1}};
static const jn_str_t jn_cut_361[] = {{"xy", 2}, {"aabaa", 5}};
static const jn_str_t jn_cut_362[] = {{"xy", 2}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_363[] = {{"xy", 2}, {"xyaab", 5}, {"xyaa", 4}};
static const jn_str_t jn_cut_364[] = {{"xy", 2}, {"axy", 3}, {"", 0}};
static const jn_str_t jn_cut_365[] = {{"xy", 2}, {"b", 1}, {"a", 1}};
static const jn_str_t jn_cut_366[] = {{"xy", 2}, {"xyxy", 4}, {"", 0}};
static const jn_str_t jn_cut_367[] = {{"xy", 2}, {"aabxyxyxy", 9}, {"b", 1}};
static const jn_str_t jn_cut_368[] = {{"xy", 2}, {"babb", 4}, {"", 0}};
static const jn_str_t jn_cut_369[] = {{"xy", 2}, {"xy", 2}, {"b", 1}};
static const jn_str_t jn_cut_370[] = {{"xy", 2}, {"xy", 2}, {"bb", 2}};
static const jn_str_t jn_cut_371[] = {{"xy", 2}, {"xyaaxya", 7}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_372[] = {{"xy", 2}, {"a", 1}, {"aaxyxya", 7}};
static const jn_str_t jn_cut_373[] = {{"xy", 2}, {"a", 1}, {"aaaab", 5}};
static const jn_str_t jn_cut_374[] = {{"xy", 2}, {"a", 1}, {"abaxy", 5}};
static const jn_str_t jn_cut_375[] = {{"xy", 2}, {"aa", 2}, {"xyb", 3}, {"", 0}};
static const jn_str_t jn_cut_376[] = {{"xy", 2}, {"aaab", 4}};
static const jn_str_t jn_cut_377[] = {{"xy", 2}, {"abb", 3}, {"b", 1}};
static const jn_str_t jn_cut_378[] = {{"xy", 2}, {"ba", 2}};
static const jn_str_t jn_cut_379[] = {{"xy", 2}, {"ba", 2}, {"", 0}};
static const jn_str_t jn_cut_380[] = {{"xy", 2}, {"bxy", 3}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_381[] = {{"xy", 2}, {"xy", 2}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_382[] = {{"xy", 2}, {"xy", 2}, {"a", 1}, {"xya", 3}, {"a", 1}};
static const jn_str_t jn_cut_383[] = {{"xya", 3}, {"", 0}};
static const jn_str_t jn_cut_384[] = {{"xya", 3}, {"a", 1}, {"xybxy", 5}};
static const jn_str_t jn_cut_385[] = {{"xya", 3}, {"a", 1}, {"abxy", 4}};
static const jn_str_t jn_cut_386[] = {{"xya", 3}, {"a", 1}, {"aaa", 3}};
static const jn_str_t jn_cut_387[] = {{"xya", 3}, {"xy", 2}};
static const jn_str_t jn_cut_388[] = {{"xya", 3}, {"xyaxy", 5}, {"bb", 2}};
static const jn_str_t jn_cut_389[] = {{"xyaa", 4}, {"axyb", 4}, {"", 0}};
static const jn_str_t jn_cut_390[] = {{"xyaaaxy", 7}, {"aa", 2}, {"aa", 2}};
static const jn_str_t jn_cut_391[] = {{"xyab", 4}};
static const jn_str_t jn_cut_392[] = {{"xyab", 4}, {"a", 1}};
static const jn_str_t jn_cut_393[] = {{"xyaxy", 5}, {"a", 1}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_394[] = {{"xyaxy", 5}, {"aaxy", 4}, {"bb", 2}};
static const jn_str_t jn_cut_395[] = {{"xyaxy", 5}, {"xy", 2}, {"a", 1}, {"xy", 2}};
static const jn_str_t jn_cut_396[] = {{"xyaxya", 6}, {"", 0}};
static const jn_str_t jn_cut_397[] = {{"xyb", 3}, {"b", 1}, {"xya", 3}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_398[] = {{"xyb", 3}, {"a", 1}, {"ab", 2}, {"ab", 2}, {"", 0}};
static const jn_str_t jn_cut_399[] = {{"xyb", 3}, {"a", 1}, {"a", 1}, {"a", 1}, {"xy", 2}, {"b", 1}};
static const jn_str_t jn_cut_400[] = {{"xyba", 4}};
static const jn_str_t jn_cut_401[] = {{"xyba", 4}, {"ab", 2}};
static const jn_str_t jn_cut_402[] = {{"xybb", 4}, {"axya", 4}};
static const jn_str_t jn_cut_403[] = {{"xybba", 5}, {"", 0}};
static const jn_str_t jn_cut_404[] = {{"xybxyaxyabb", 11}};
static const jn_str_t jn_cut_405[] = {{"xybxyxyba", 9}, {"xy", 2}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_406[] = {{"xyxy", 4}};
static const jn_str_t jn_cut_407[] = {{"xyxy", 4}, {"xyaxyaxyxy", 10}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_408[] = {{"xyxy", 4}, {"", 0}};
static const jn_str_t jn_cut_409[] = {{"xyxy", 4}, {"b", 1}};
static const jn_str_t jn_cut_410[] = {{"xyxy", 4}, {"abxy", 4}, {"", 0}};
static const jn_str_t jn_cut_411[] = {{"xyxyb", 5}, {"b", 1}, {"xy", 2}, {"a", 1}};
static const jn_str_t jn_cut_412[] = {{"a", 1}, {"b", 1}};
static const jn_str_t jn_cut_414[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_415[] = {{"a", 1}};
static const jn_str_t jn_cut_416[] = {{"a", 1}, {"b", 1}};
static const jn_str_t jn_cut_418[] = {{"x", 1}, {"y", 1}};
static const jn_str_t jn_cut_420[] = {{"a", 1}, {"b", 1}, {"c", 1}, {"d", 1}};
static const jn_str_t jn_cut_424[] = {{"b", 1}, {"xy", 2}, {"xybb", 4}};
static const jn_str_t jn_cut_425[] = {{"xyxy", 4}};
static const jn_str_t jn_cut_426[] = {{"bxy", 3}};
static const jn_str_t jn_cut_427[] = {{"b", 1}, {"", 0}};
static const jn_str_t jn_cut_428[] = {{"b", 1}, {"xyxy", 4}, {"b", 1}};
static const jn_str_t jn_cut_429[] = {{"xyaxy", 5}, {"xya", 3}};
static const jn_str_t jn_cut_430[] = {{"xybxyxy", 7}, {"b", 1}};
static const jn_str_t jn_cut_432[] = {{"a", 1}, {"aa", 2}, {"", 0}};
static const jn_str_t jn_cut_433[] = {{"ab", 2}, {"a", 1}, {"axy", 3}};
static const jn_str_t jn_cut_434[] = {{"axy", 3}};
static const jn_str_t jn_cut_435[] = {{"b", 1}, {"axyxy", 5}};
static const jn_str_t jn_cut_436[] = {{"bxyxyxy", 7}, {"xy", 2}};
static const jn_str_t jn_cut_437[] = {{"aa", 2}, {"b", 1}};
static const jn_str_t jn_cut_438[] = {{"axy", 3}, {"a", 1}};
static const jn_str_t jn_cut_439[] = {{"ba", 2}, {"", 0}};
static const jn_str_t jn_cut_440[] = {{"xy", 2}};
static const jn_str_t jn_cut_441[] = {{"xy", 2}, {"ba", 2}, {"a", 1}};
static const jn_str_t jn_cut_442[] = {{"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_443[] = {{"xy", 2}, {"a", 1}};
static const jn_str_t jn_cut_444[] = {{"xy", 2}, {"bb", 2}, {"xya", 3}};
static const jn_str_t jn_cut_445[] = {{"xy", 2}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_446[] = {{"xya", 3}, {"ba", 2}, {"b", 1}};
static const jn_str_t jn_cut_447[] = {{"xyab", 4}};
static const jn_str_t jn_cut_448[] = {{"xyba", 4}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_451[] = {{"b", 1}, {"", 0}};
static const jn_str_t jn_cut_452[] = {{"xyaxyxy", 7}};
static const jn_str_t jn_cut_453[] = {{"xyxy", 4}};
static const jn_str_t jn_cut_455[] = {{"xya", 3}, {"xyb", 3}};
static const jn_str_t jn_cut_456[] = {{"baaa", 4}};
static const jn_str_t jn_cut_457[] = {{"aa", 2}, {"a", 1}, {"b", 1}};
static const jn_str_t jn_cut_458[] = {{"xyb", 3}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_459[] = {{"bb", 2}, {"a", 1}, {"xy", 2}};
static const jn_str_t jn_cut_460[] = {{"a", 1}, {"aaa", 3}, {"", 0}};
static const jn_str_t jn_cut_461[] = {{"xy", 2}, {"a", 1}};
static const jn_str_t jn_cut_462[] = {{"a", 1}, {"xy", 2}};
static const jn_str_t jn_cut_463[] = {{"ab", 2}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_464[] = {{"b", 1}};
static const jn_str_t jn_cut_465[] = {{"ba", 2}, {"b", 1}};
static const jn_str_t jn_cut_466[] = {{"bb", 2}, {"aab", 3}};
static const jn_str_t jn_cut_467[] = {{"xy", 2}, {"xy", 2}, {"xya", 3}};
static const jn_str_t jn_cut_468[] = {{"xyxya", 5}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_472[] = {{"xyxyxy", 6}};
static const jn_str_t jn_cut_473[] = {{"a", 1}};
static const jn_str_t jn_cut_474[] = {{"xyxyxy", 6}, {"bxya", 4}, {"", 0}};
static const jn_str_t jn_cut_476[] = {{"ba", 2}, {"a", 1}};
static const jn_str_t jn_cut_477[] = {{"b", 1}};
static const jn_str_t jn_cut_478[] = {{"b", 1}, {"", 0}};
static const jn_str_t jn_cut_479[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_480[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_481[] = {{"aaxya", 5}, {"", 0}};
static const jn_str_t jn_cut_482[] = {{"aaxyb", 5}};
static const jn_str_t jn_cut_483[] = {{"ab", 2}, {"", 0}};
static const jn_str_t jn_cut_484[] = {{"axy", 3}};
static const jn_str_t jn_cut_485[] = {{"b", 1}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_486[] = {{"ba", 2}, {"b", 1}};
static const jn_str_t jn_cut_487[] = {{"bb", 2}, {"ab", 2}, {"xy", 2}};
static const jn_str_t jn_cut_488[] = {{"bxy", 3}, {"a", 1}};
static const jn_str_t jn_cut_489[] = {{"bxyb", 4}, {"", 0}};
static const jn_str_t jn_cut_490[] = {{"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_491[] = {{"xya", 3}, {"b", 1}};
static const jn_str_t jn_cut_492[] = {{"xya", 3}, {"aa", 2}, {"", 0}};
static const jn_str_t jn_cut_493[] = {{"a", 1}};
static const jn_str_t jn_cut_494[] = {{"a", 1}, {"bb", 2}, {"", 0}};
static const jn_str_t jn_cut_495[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_496[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_497[] = {{"a", 1}, {"b", 1}, {"b", 1}, {"bb", 2}};
static const jn_str_t jn_cut_498[] = {{"a", 1}, {"bb", 2}, {"axyxy", 5}};
static const jn_str_t jn_cut_499[] = {{"a", 1}, {"bxy", 3}, {"xy", 2}};
static const jn_str_t jn_cut_500[] = {{"a", 1}, {"axya", 4}};
static const jn_str_t jn_cut_501[] = {{"a", 1}, {"xyaaxyb", 7}};
static const jn_str_t jn_cut_502[] = {{"a", 1}, {"a", 1}, {"xyb", 3}};
static const jn_str_t jn_cut_503[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_504[] = {{"a", 1}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_505[] = {{"a", 1}, {"ab", 2}};
static const jn_str_t jn_cut_506[] = {{"a", 1}, {"ba", 2}, {"b", 1}};
static const jn_str_t jn_cut_507[] = {{"a", 1}, {"axy", 3}, {"aa", 2}, {"b", 1}};
static const jn_str_t jn_cut_508[] = {{"a", 1}, {"bxy", 3}, {"", 0}};
static const jn_str_t jn_cut_509[] = {{"aa", 2}};
static const jn_str_t jn_cut_510[] = {{"aa", 2}, {"", 0}};
static const jn_str_t jn_cut_511[] = {{"aa", 2}, {"xy", 2}, {"a", 1}};
static const jn_str_t jn_cut_512[] = {{"aaa", 3}, {"ab", 2}};
static const jn_str_t jn_cut_513[] = {{"aab", 3}};
static const jn_str_t jn_cut_514[] = {{"aaba", 4}, {"", 0}};
static const jn_str_t jn_cut_515[] = {{"aaxyb", 5}};
static const jn_str_t jn_cut_516[] = {{"ab", 2}, {"", 0}};
static const jn_str_t jn_cut_517[] = {{"abb", 3}};
static const jn_str_t jn_cut_518[] = {{"abb", 3}, {"abxybb", 6}};
static const jn_str_t jn_cut_519[] = {{"axy", 3}, {"xy", 2}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_520[] = {{"axy", 3}, {"xyxya", 5}, {"xy", 2}};
static const jn_str_t jn_cut_521[] = {{"axyxy", 5}};
static const jn_str_t jn_cut_522[] = {{"b", 1}};
static const jn_str_t jn_cut_523[] = {{"b", 1}, {"xyxyaxyb", 8}};
static const jn_str_t jn_cut_524[] = {{"b", 1}, {"bbaa", 4}};
static const jn_str_t jn_cut_525[] = {{"b", 1}, {"xyxyxybxy", 9}};
static const jn_str_t jn_cut_526[] = {{"b", 1}, {"xyb", 3}};
static const jn_str_t jn_cut_527[] = {{"b", 1}, {"axy", 3}};
static const jn_str_t jn_cut_528[] = {{"b", 1}, {"a", 1}};
static const jn_str_t jn_cut_529[] = {{"b", 1}, {"", 0}};
static const jn_str_t jn_cut_530[] = {{"b", 1}, {"aa", 2}, {"b", 1}};
static const jn_str_t jn_cut_531[] = {{"b", 1}, {"", 0}};
static const jn_str_t jn_cut_532[] = {{"b", 1}, {"ab", 2}, {"xy", 2}, {"xy", 2}};
static const jn_str_t jn_cut_533[] = {{"ba", 2}, {"", 0}};
static const jn_str_t jn_cut_534[] = {{"ba", 2}, {"axyba", 5}, {"", 0}};
static const jn_str_t jn_cut_535[] = {{"ba", 2}, {"", 0}};
static const jn_str_t jn_cut_536[] = {{"ba", 2}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_537[] = {{"baa", 3}, {"", 0}};
static const jn_str_t jn_cut_538[] = {{"baxy", 4}, {"", 0}};
static const jn_str_t jn_cut_539[] = {{"bb", 2}, {"b", 1}, {"xya", 3}};
static const jn_str_t jn_cut_540[] = {{"bbxya", 5}, {"xy", 2}};
static const jn_str_t jn_cut_541[] = {{"bxy", 3}, {"xy", 2}, {"xy", 2}};
static const jn_str_t jn_cut_542[] = {{"bxy", 3}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_543[] = {{"bxyxy", 5}, {"", 0}};
static const jn_str_t jn_cut_544[] = {{"bxyxyb", 6}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_545[] = {{"xy", 2}};
static const jn_str_t jn_cut_546[] = {{"xy", 2}, {"a", 1}};
static const jn_str_t jn_cut_547[] = {{"xy", 2}, {"xya", 3}, {"bxybxy", 6}};
static const jn_str_t jn_cut_548[] = {{"xy", 2}, {"xyb", 3}, {"bb", 2}, {"", 0}};
static const jn_str_t jn_cut_549[] = {{"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_550[] = {{"xy", 2}, {"xy", 2}, {"bab", 3}, {"a", 1}};
static const jn_str_t jn_cut_551[] = {{"xy", 2}, {"babxy", 5}};
static const jn_str_t jn_cut_552[] = {{"xy", 2}, {"xy", 2}, {"b", 1}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_553[] = {{"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_554[] = {{"xy", 2}, {"abxyxy", 6}, {"xy", 2}};
static const jn_str_t jn_cut_555[] = {{"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_556[] = {{"xy", 2}, {"a", 1}};
static const jn_str_t jn_cut_557[] = {{"xy", 2}, {"aa", 2}};
static const jn_str_t jn_cut_558[] = {{"xya", 3}, {"a", 1}};
static const jn_str_t jn_cut_559[] = {{"xyaa", 4}, {"b", 1}, {"axyxy", 5}};
static const jn_str_t jn_cut_560[] = {{"xyabxy", 6}};
static const jn_str_t jn_cut_561[] = {{"xyaxyxy", 7}};
static const jn_str_t jn_cut_562[] = {{"xyb", 3}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_563[] = {{"xyb", 3}, {"ab", 2}, {"xy", 2}};
static const jn_str_t jn_cut_564[] = {{"xyb", 3}, {"", 0}};
static const jn_str_t jn_cut_565[] = {{"xyb", 3}, {"aa", 2}};
static const jn_str_t jn_cut_566[] = {{"xybxyxyxyxyaba", 14}};
static const jn_str_t jn_cut_567[] = {{"xyxya", 5}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_568[] = {{"xyxybxyb", 8}};
static const jn_str_t jn_cut_569[] = {{"xyxyxy", 6}, {"a", 1}, {"xyxya", 5}};
static const jn_str_t jn_cut_571[] = {{"", 0}, {"", 0}};
static const jn_str_t jn_cut_572[] = {{"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_573[] = {{"a", 1}};
static const jn_str_t jn_cut_574[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_575[] = {{"", 0}, {"a", 1}};
static const jn_str_t jn_cut_576[] = {{"a", 1}, {"b", 1}};
static const jn_str_t jn_cut_577[] = {{"a", 1}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_578[] = {{" ", 1}};
static const jn_str_t jn_cut_579[] = {{"  ", 2}};
static const jn_str_t jn_cut_580[] = {{"  ", 2}, {" ", 1}, {"", 0}, {"a", 1}};
static const jn_str_t jn_cut_581[] = {{"  bab", 5}, {"", 0}, {"abxy", 4}};
static const jn_str_t jn_cut_582[] = {{" ", 1}, {"", 0}, {"", 0}, {"", 0}, {"b", 1}, {"", 0}, {" ", 1}, {"", 0}};
static const jn_str_t jn_cut_583[] = {{" ", 1}, {"", 0}, {"", 0}, {"a b ", 4}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_584[] = {{" ", 1}, {"", 0}, {"", 0}, {"a", 1}, {" ", 1}, {"", 0}};
static const jn_str_t jn_cut_585[] = {{" ", 1}, {"", 0}, {"bb", 2}, {"", 0}};
static const jn_str_t jn_cut_586[] = {{" ", 1}, {"xyaxy", 5}, {"", 0}, {"bb ", 3}};
static const jn_str_t jn_cut_587[] = {{" a", 2}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"axy", 3}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_588[] = {{" a", 2}, {"", 0}, {"", 0}, {"xya", 3}};
static const jn_str_t jn_cut_589[] = {{" aab", 4}};
static const jn_str_t jn_cut_590[] = {{" b", 2}};
static const jn_str_t jn_cut_591[] = {{" ba", 3}};
static const jn_str_t jn_cut_592[] = {{" bxy", 4}, {"", 0}, {"abb", 3}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_593[] = {{"", 0}, {" ", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_594[] = {{"", 0}, {" b", 2}};
static const jn_str_t jn_cut_595[] = {{"", 0}, {"", 0}, {" ", 1}, {"", 0}, {"axy a ", 6}};
static const jn_str_t jn_cut_596[] = {{"", 0}, {"", 0}, {" xyxyaxy", 8}, {"", 0}, {"xy", 2}};
static const jn_str_t jn_cut_597[] = {{"", 0}, {"", 0}, {"", 0}, {" ", 1}, {"", 0}, {"xya", 3}, {"", 0}, {" ", 1}};
static const jn_str_t jn_cut_598[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_599[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {" ", 1}};
static const jn_str_t jn_cut_600[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"  a", 3}, {"xy", 2}};
static const jn_str_t jn_cut_601[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"  xy", 4}, {"", 0}, {"", 0}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_602[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"xy", 2}};
static const jn_str_t jn_cut_603[] = {{"", 0}, {"", 0}, {"", 0}, {"a", 1}, {"", 0}, {" ba b", 5}};
static const jn_str_t jn_cut_604[] = {{"", 0}, {"", 0}, {"", 0}, {"b bxyaxyxya", 11}};
static const jn_str_t jn_cut_605[] = {{"", 0}, {"", 0}, {"", 0}, {"baxyb xy", 8}, {"xy", 2}};
static const jn_str_t jn_cut_606[] = {{"", 0}, {"", 0}, {"", 0}, {"xy  xyb", 7}};
static const jn_str_t jn_cut_607[] = {{"", 0}, {"", 0}, {"", 0}, {"xy", 2}, {"bxy", 3}};
static const jn_str_t jn_cut_608[] = {{"", 0}, {"", 0}, {"", 0}, {"xyaxy", 5}};
static const jn_str_t jn_cut_609[] = {{"", 0}, {"", 0}, {"a", 1}};
static const jn_str_t jn_cut_610[] = {{"", 0}, {"", 0}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_611[] = {{"", 0}, {"", 0}, {"aaabxy", 6}, {"xy", 2}, {" ", 1}};
static const jn_str_t jn_cut_612[] = {{"", 0}, {"", 0}, {"abxy ", 5}, {"", 0}};
static const jn_str_t jn_cut_613[] = {{"", 0}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_614[] = {{"", 0}, {"", 0}, {"b  xy", 5}, {"b", 1}, {"  ", 2}};
static const jn_str_t jn_cut_615[] = {{"", 0}, {"", 0}, {"b xy", 4}};
static const jn_str_t jn_cut_616[] = {{"", 0}, {"", 0}, {"b xy", 4}, {"", 0}, {"xy", 2}};
static const jn_str_t jn_cut_617[] = {{"", 0}, {"", 0}, {"bxya", 4}, {"b", 1}};
static const jn_str_t jn_cut_618[] = {{"", 0}, {"", 0}, {"bxyxy ", 6}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_619[] = {{"", 0}, {"", 0}, {"bxyxyxy  a", 10}};
static const jn_str_t jn_cut_620[] = {{"", 0}, {"", 0}, {"xy a", 4}, {"a  ", 3}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_621[] = {{"", 0}, {"", 0}, {"xy", 2}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {" axy", 4}};
static const jn_str_t jn_cut_622[] = {{"", 0}, {"", 0}, {"xy", 2}, {"", 0}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_623[] = {{"", 0}, {"", 0}, {"xy", 2}, {"xy", 2}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_624[] = {{"", 0}, {"b", 1}};
static const jn_str_t jn_cut_625[] = {{"", 0}, {"b ", 2}, {"", 0}, {"  b", 3}};
static const jn_str_t jn_cut_626[] = {{"", 0}, {"b", 1}, {"xyxy", 4}};
static const jn_str_t jn_cut_627[] = {{"", 0}, {"xy", 2}};
static const jn_str_t jn_cut_628[] = {{"", 0}, {"xybb", 4}};
static const jn_str_t jn_cut_629[] = {{"a b", 3}, {"ab ", 3}};
static const jn_str_t jn_cut_630[] = {{"a ba", 4}};
static const jn_str_t jn_cut_631[] = {{"a xy b", 6}};
static const jn_str_t jn_cut_632[] = {{"a", 1}, {"", 0}, {"", 0}, {" xyb ", 5}};
static const jn_str_t jn_cut_633[] = {{"a", 1}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_634[] = {{"a", 1}, {"", 0}, {"xya", 3}, {"", 0}, {"", 0}, {"xy", 2}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_635[] = {{"aa", 2}, {" a", 2}};
static const jn_str_t jn_cut_636[] = {{"aa", 2}, {"ba", 2}};
static const jn_str_t jn_cut_637[] = {{"aaa", 3}};
static const jn_str_t jn_cut_638[] = {{"aabab", 5}, {"", 0}};
static const jn_str_t jn_cut_639[] = {{"ab ", 3}, {"", 0}};
static const jn_str_t jn_cut_640[] = {{"ab", 2}, {"", 0}, {"", 0}, {"", 0}, {" ", 1}, {" ", 1}};
static const jn_str_t jn_cut_641[] = {{"b", 1}};
static const jn_str_t jn_cut_642[] = {{"b  bbxy", 7}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_643[] = {{"b b", 3}};
static const jn_str_t jn_cut_644[] = {{"b b", 3}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_645[] = {{"b xyxyxy", 8}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_646[] = {{"b", 1}, {" xyxyxyxyaxy", 12}};
static const jn_str_t jn_cut_647[] = {{"b", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_648[] = {{"b", 1}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"bb ab", 5}};
static const jn_str_t jn_cut_649[] = {{"b", 1}, {"", 0}, {"", 0}, {"", 0}, {"b", 1}, {"", 0}, {" xybxy", 6}};
static const jn_str_t jn_cut_650[] = {{"b", 1}, {"", 0}, {"a", 1}, {"", 0}, {"", 0}, {"bxyxyxy", 7}};
static const jn_str_t jn_cut_651[] = {{"b", 1}, {"", 0}, {"xy", 2}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_652[] = {{"b", 1}, {"", 0}, {"xy", 2}, {"", 0}, {"xyb ", 4}};
static const jn_str_t jn_cut_653[] = {{"b", 1}, {"a ", 2}, {"a", 1}, {"", 0}, {"xy a", 4}};
static const jn_str_t jn_cut_654[] = {{"b", 1}, {"xyxyxy", 6}, {"", 0}, {" ", 1}, {"", 0}};
static const jn_str_t jn_cut_655[] = {{"ba", 2}};
static const jn_str_t jn_cut_656[] = {{"ba", 2}, {"", 0}, {"", 0}, {" ", 1}};
static const jn_str_t jn_cut_657[] = {{"ba", 2}, {"axy", 3}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_658[] = {{"baaa", 4}, {"axy", 3}, {"", 0}};
static const jn_str_t jn_cut_659[] = {{"baab", 4}};
static const jn_str_t jn_cut_660[] = {{"bb", 2}, {"", 0}, {"xy", 2}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_661[] = {{"bbb", 3}};
static const jn_str_t jn_cut_662[] = {{"bxyb", 4}, {"a", 1}};
static const jn_str_t jn_cut_663[] = {{"xy", 2}};
static const jn_str_t jn_cut_664[] = {{"xy ", 3}, {"", 0}};
static const jn_str_t jn_cut_665[] = {{"xy", 2}, {" a", 2}};
static const jn_str_t jn_cut_666[] = {{"xy", 2}, {" bb", 3}, {"xy", 2}};
static const jn_str_t jn_cut_667[] = {{"xy", 2}, {"", 0}, {" baaxya ", 8}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_668[] = {{"xy", 2}, {"", 0}, {"", 0}, {"  a", 3}};
static const jn_str_t jn_cut_669[] = {{"xy", 2}, {"", 0}, {"", 0}, {"  axy", 5}, {"", 0}};
static const jn_str_t jn_cut_670[] = {{"xy", 2}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"xy", 2}};
static const jn_str_t jn_cut_671[] = {{"xy", 2}, {"", 0}, {"", 0}, {"b", 1}, {"", 0}, {"", 0}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_672[] = {{"xy", 2}, {"", 0}, {"", 0}, {"xyba", 4}, {"", 0}, {"", 0}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_673[] = {{"xy", 2}, {"", 0}, {"a", 1}, {"", 0}, {"a", 1}};
static const jn_str_t jn_cut_674[] = {{"xy", 2}, {"", 0}, {"aa", 2}};
static const jn_str_t jn_cut_675[] = {{"xy", 2}, {"", 0}, {"bbxy", 4}};
static const jn_str_t jn_cut_676[] = {{"xy", 2}, {"aa", 2}};
static const jn_str_t jn_cut_677[] = {{"xy", 2}, {"b", 1}};
static const jn_str_t jn_cut_678[] = {{"xy", 2}, {"b", 1}, {"", 0}, {" a", 2}, {"", 0}, {"a", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_679[] = {{"xy", 2}, {"ba", 2}, {"a", 1}};
static const jn_str_t jn_cut_680[] = {{"xy", 2}, {"xy", 2}, {"", 0}, {"a", 1}};
static const jn_str_t jn_cut_681[] = {{"xya ", 4}, {"", 0}, {"b", 1}, {"", 0}, {"", 0}, {"", 0}, {"xy", 2}};
static const jn_str_t jn_cut_682[] = {{"xya b", 5}, {"", 0}, {"a", 1}, {" b", 2}};
static const jn_str_t jn_cut_683[] = {{"xya", 3}, {"", 0}};
static const jn_str_t jn_cut_684[] = {{"xyaxy a", 7}, {"", 0}, {"  ", 2}, {"", 0}, {"a", 1}};
static const jn_str_t jn_cut_685[] = {{"xyaxyb", 6}};
static const jn_str_t jn_cut_686[] = {{"xyb", 3}};
static const jn_str_t jn_cut_687[] = {{"xyb aa", 6}, {"", 0}, {"a", 1}};
static const jn_str_t jn_cut_688[] = {{"xyba", 4}, {"xy a", 4}, {"", 0}};
static const jn_str_t jn_cut_689[] = {{"xybb", 4}};
static const jn_str_t jn_cut_690[] = {{"xybxyb ", 7}};
static const jn_str_t jn_cut_691[] = {{"xyxy", 4}, {" ", 1}, {"", 0}, {"", 0}, {" ", 1}, {"", 0}};
static const jn_str_t jn_cut_692[] = {{"xyxy", 4}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_693[] = {{"xyxy", 4}, {"", 0}, {"", 0}, {"b", 1}, {"", 0}, {"", 0}, {"a", 1}, {" ", 1}};
static const jn_str_t jn_cut_695[] = {{"", 0}, {"", 0}};
static const jn_str_t jn_cut_696[] = {{"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_697[] = {{"a", 1}};
static const jn_str_t jn_cut_698[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_699[] = {{"", 0}, {"a", 1}};
static const jn_str_t jn_cut_700[] = {{"a", 1}, {"b", 1}};
static const jn_str_t jn_cut_701[] = {{"a", 1}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_702[] = {{"\011", 1}};
static const jn_str_t jn_cut_703[] = {{"\011\011\011a", 4}};
static const jn_str_t jn_cut_704[] = {{"\011\011", 2}, {"\011bxy\011", 5}, {"", 0}};
static const jn_str_t jn_cut_705[] = {{"\011\011", 2}, {"ba", 2}, {"xy", 2}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_706[] = {{"\011", 1}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_707[] = {{"\011", 1}, {"", 0}, {"xy\011\011", 4}, {"b\011", 2}};
static const jn_str_t jn_cut_708[] = {{"\011", 1}, {"", 0}, {"xyaxybb", 7}};
static const jn_str_t jn_cut_709[] = {{"\011a", 2}, {"", 0}, {"", 0}, {"", 0}, {"axy", 3}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_710[] = {{"\011aba", 4}};
static const jn_str_t jn_cut_711[] = {{"\011abb", 4}};
static const jn_str_t jn_cut_712[] = {{"\011bb", 3}, {"", 0}, {"xya", 3}};
static const jn_str_t jn_cut_713[] = {{"\011xy", 3}, {"", 0}, {"", 0}, {"bxyb\011", 5}, {"", 0}};
static const jn_str_t jn_cut_714[] = {{"\011xy", 3}, {"", 0}, {"b", 1}, {"a", 1}};
static const jn_str_t jn_cut_715[] = {{"\011xya", 4}, {"", 0}, {"\011\011a", 3}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_716[] = {{"\011xyxy", 5}, {"bxy", 3}, {"", 0}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_717[] = {{"", 0}, {"\011\011", 2}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_718[] = {{"", 0}, {"\011xya", 4}};
static const jn_str_t jn_cut_719[] = {{"", 0}, {"", 0}, {"\011", 1}, {"", 0}, {"ba\011", 3}, {"", 0}, {"", 0}, {"", 0}, {"a", 1}};
static const jn_str_t jn_cut_720[] = {{"", 0}, {"", 0}, {"\011xyxyb", 6}, {"", 0}, {"xyb", 3}};
static const jn_str_t jn_cut_721[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_722[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"\011axya", 5}, {"", 0}};
static const jn_str_t jn_cut_723[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"axyb", 4}};
static const jn_str_t jn_cut_724[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"b", 1}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_725[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"b", 1}, {"", 0}, {"a", 1}, {"xya", 3}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_726[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"xy", 2}, {"", 0}, {"", 0}, {"xyxy", 4}};
static const jn_str_t jn_cut_727[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"xyxyxybxybxy", 12}};
static const jn_str_t jn_cut_728[] = {{"", 0}, {"", 0}, {"", 0}, {"a", 1}, {"", 0}, {"", 0}, {"", 0}, {"bxyaxy", 6}};
static const jn_str_t jn_cut_729[] = {{"", 0}, {"", 0}, {"", 0}, {"ab", 2}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_730[] = {{"", 0}, {"", 0}, {"", 0}, {"b", 1}, {"xy", 2}};
static const jn_str_t jn_cut_731[] = {{"", 0}, {"", 0}, {"", 0}, {"xyxyaxy", 7}, {"", 0}};
static const jn_str_t jn_cut_732[] = {{"", 0}, {"", 0}, {"a", 1}};
static const jn_str_t jn_cut_733[] = {{"", 0}, {"", 0}, {"a", 1}, {"", 0}, {"", 0}, {"\011", 1}};
static const jn_str_t jn_cut_734[] = {{"", 0}, {"", 0}, {"a", 1}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_735[] = {{"", 0}, {"", 0}, {"axyxy", 5}, {"\011", 1}};
static const jn_str_t jn_cut_736[] = {{"", 0}, {"", 0}, {"b\011\011", 3}, {"", 0}, {"xyaa\011", 5}};
static const jn_str_t jn_cut_737[] = {{"", 0}, {"", 0}, {"b", 1}, {"", 0}, {"baxy\011", 5}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_738[] = {{"", 0}, {"", 0}, {"bab", 3}};
static const jn_str_t jn_cut_739[] = {{"", 0}, {"", 0}, {"bxy\011\011", 5}, {"a\011", 2}};
static const jn_str_t jn_cut_740[] = {{"", 0}, {"", 0}, {"bxyxy", 5}};
static const jn_str_t jn_cut_741[] = {{"", 0}, {"", 0}, {"xyaxy", 5}, {"\011bb", 3}};
static const jn_str_t jn_cut_742[] = {{"", 0}, {"", 0}, {"xyb", 3}};
static const jn_str_t jn_cut_743[] = {{"", 0}, {"", 0}, {"xyxy", 4}, {"", 0}, {"b", 1}, {"", 0}, {"\011\011", 2}, {"", 0}};
static const jn_str_t jn_cut_744[] = {{"", 0}, {"a\011", 2}};
static const jn_str_t jn_cut_745[] = {{"", 0}, {"a\011", 2}, {"", 0}, {"aba", 3}};
static const jn_str_t jn_cut_746[] = {{"", 0}, {"a", 1}, {"", 0}, {"\011bxy", 4}};
static const jn_str_t jn_cut_747[] = {{"", 0}, {"a", 1}, {"", 0}, {"", 0}, {"", 0}, {"xy", 2}};
static const jn_str_t jn_cut_748[] = {{"", 0}, {"b", 1}, {"\011", 1}, {"bbxy", 4}, {"xy", 2}};
static const jn_str_t jn_cut_749[] = {{"", 0}, {"b", 1}, {"", 0}, {"axy", 3}};
static const jn_str_t jn_cut_750[] = {{"", 0}, {"ba", 2}, {"a", 1}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_751[] = {{"", 0}, {"bba", 3}, {"", 0}, {"\011", 1}};
static const jn_str_t jn_cut_752[] = {{"", 0}, {"xy", 2}, {"", 0}, {"", 0}, {"", 0}, {"\011", 1}, {"", 0}};
static const jn_str_t jn_cut_753[] = {{"", 0}, {"xy", 2}, {"xybxy", 5}};
static const jn_str_t jn_cut_754[] = {{"", 0}, {"xya\011xy", 6}};
static const jn_str_t jn_cut_755[] = {{"a\011", 2}};
static const jn_str_t jn_cut_756[] = {{"a\011", 2}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_757[] = {{"a\011", 2}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_758[] = {{"a\011xybbb", 7}};
static const jn_str_t jn_cut_759[] = {{"a\011xybbxy\011b\011", 11}};
static const jn_str_t jn_cut_760[] = {{"a\011xybxy", 7}, {"", 0}, {"\011", 1}};
static const jn_str_t jn_cut_761[] = {{"a", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_762[] = {{"a", 1}, {"", 0}, {"\011\011xyb", 5}};
static const jn_str_t jn_cut_763[] = {{"a", 1}, {"", 0}, {"", 0}, {"", 0}, {"aaaa", 4}, {"", 0}};
static const jn_str_t jn_cut_764[] = {{"a", 1}, {"", 0}, {"", 0}, {"", 0}, {"xy\011", 3}, {"a\011", 2}};
static const jn_str_t jn_cut_765[] = {{"a", 1}, {"", 0}, {"a", 1}, {"\011a", 2}, {"\011\011", 2}};
static const jn_str_t jn_cut_766[] = {{"a", 1}, {"", 0}, {"axyxyxy", 7}, {"xya", 3}};
static const jn_str_t jn_cut_767[] = {{"a", 1}, {"aaa\011", 4}};
static const jn_str_t jn_cut_768[] = {{"a", 1}, {"xyaxyba", 7}, {"xy\011", 3}};
static const jn_str_t jn_cut_769[] = {{"aa", 2}};
static const jn_str_t jn_cut_770[] = {{"aa", 2}, {"", 0}, {"xyb", 3}};
static const jn_str_t jn_cut_771[] = {{"aab", 3}, {"", 0}, {"\011", 1}, {"", 0}, {"\011b", 2}};
static const jn_str_t jn_cut_772[] = {{"ab\011", 3}};
static const jn_str_t jn_cut_773[] = {{"ab\011bxy", 6}};
static const jn_str_t jn_cut_774[] = {{"aba", 3}, {"", 0}, {"abb\011xya", 7}};
static const jn_str_t jn_cut_775[] = {{"abbb", 4}, {"", 0}, {"", 0}, {"xy", 2}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_776[] = {{"abxya", 5}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_777[] = {{"axy", 3}, {"", 0}};
static const jn_str_t jn_cut_778[] = {{"axy", 3}, {"", 0}, {"\011", 1}};
static const jn_str_t jn_cut_779[] = {{"axy", 3}, {"", 0}, {"xy", 2}};
static const jn_str_t jn_cut_780[] = {{"axy", 3}, {"a", 1}, {"a", 1}};
static const jn_str_t jn_cut_781[] = {{"axy", 3}, {"xy", 2}, {"", 0}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_782[] = {{"axyb", 4}};
static const jn_str_t jn_cut_783[] = {{"axyb\011", 5}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_784[] = {{"axyxy", 5}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_785[] = {{"b", 1}};
static const jn_str_t jn_cut_786[] = {{"b\011", 2}, {"", 0}, {"", 0}, {"xy\011\011", 4}};
static const jn_str_t jn_cut_787[] = {{"b\011xyxyb", 7}, {"", 0}, {"xy", 2}, {"\011xy", 3}};
static const jn_str_t jn_cut_788[] = {{"b", 1}, {"", 0}};
static const jn_str_t jn_cut_789[] = {{"b", 1}, {"\011\011", 2}};
static const jn_str_t jn_cut_790[] = {{"b", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_791[] = {{"b", 1}, {"", 0}, {"\011\011", 2}};
static const jn_str_t jn_cut_792[] = {{"b", 1}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_793[] = {{"b", 1}, {"", 0}, {"", 0}, {"b", 1}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_794[] = {{"ba", 2}};
static const jn_str_t jn_cut_795[] = {{"ba\011", 3}, {"", 0}, {"\011b", 2}, {"ba", 2}};
static const jn_str_t jn_cut_796[] = {{"ba", 2}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_797[] = {{"baxyxy", 6}};
static const jn_str_t jn_cut_798[] = {{"bb", 2}};
static const jn_str_t jn_cut_799[] = {{"bb\011", 3}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_800[] = {{"bb", 2}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_801[] = {{"bxy", 3}, {"", 0}, {"", 0}, {"", 0}, {"xyaa\011", 5}};
static const jn_str_t jn_cut_802[] = {{"bxya", 4}};
static const jn_str_t jn_cut_803[] = {{"bxyxy", 5}};
static const jn_str_t jn_cut_804[] = {{"xy", 2}};
static const jn_str_t jn_cut_805[] = {{"xy\011ab", 5}, {"", 0}, {"xyxy", 4}, {"", 0}};
static const jn_str_t jn_cut_806[] = {{"xy", 2}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_807[] = {{"xy", 2}, {"", 0}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_808[] = {{"xy", 2}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_809[] = {{"xyaaa", 5}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_810[] = {{"xyaxyaab", 8}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_811[] = {{"xyb", 3}};
static const jn_str_t jn_cut_812[] = {{"xybb", 4}, {"xyb", 3}, {"", 0}, {"xyb", 3}};
static const jn_str_t jn_cut_813[] = {{"xyxy", 4}, {"", 0}, {"aaxy", 4}};
static const jn_str_t jn_cut_814[] = {{"xyxyxy", 6}, {"a", 1}};
static const jn_str_t jn_cut_815[] = {{"xyxyxya", 7}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_817[] = {{"", 0}, {"", 0}};
static const jn_str_t jn_cut_818[] = {{"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_819[] = {{"a", 1}};
static const jn_str_t jn_cut_820[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_821[] = {{"", 0}, {"a", 1}};
static const jn_str_t jn_cut_822[] = {{"a", 1}, {"b", 1}};
static const jn_str_t jn_cut_823[] = {{"a", 1}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_824[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {" b", 2}, {"b  ", 3}};
static const jn_str_t jn_cut_825[] = {{"", 0}, {"", 0}, {"", 0}, {"bbxya", 5}, {"", 0}};
static const jn_str_t jn_cut_826[] = {{"", 0}, {"", 0}, {" xyxy", 5}, {" xy ", 4}};
static const jn_str_t jn_cut_827[] = {{"", 0}, {"", 0}, {"axybxy", 6}};
static const jn_str_t jn_cut_828[] = {{"", 0}, {"", 0}, {"bbxy", 4}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_829[] = {{"", 0}, {"", 0}, {"xy", 2}, {"bbba", 4}};
static const jn_str_t jn_cut_830[] = {{"", 0}, {"  ", 2}, {"aba", 3}, {" ", 1}};
static const jn_str_t jn_cut_831[] = {{"", 0}, {"  b", 3}, {"", 0}};
static const jn_str_t jn_cut_832[] = {{"", 0}, {" a ", 3}};
static const jn_str_t jn_cut_833[] = {{"", 0}, {" axyxybb  ", 10}};
static const jn_str_t jn_cut_834[] = {{"", 0}, {"a", 1}, {" aa aa ", 7}};
static const jn_str_t jn_cut_835[] = {{"", 0}, {"aaaxyxyaxy", 10}};
static const jn_str_t jn_cut_836[] = {{"", 0}, {"axy", 3}, {" xy", 3}};
static const jn_str_t jn_cut_837[] = {{"", 0}, {"axya", 4}};
static const jn_str_t jn_cut_838[] = {{"", 0}, {"b ", 2}, {"", 0}, {" a", 2}};
static const jn_str_t jn_cut_839[] = {{"", 0}, {"b   xy", 6}};
static const jn_str_t jn_cut_840[] = {{"", 0}, {"ba", 2}, {"xyaaxy", 6}};
static const jn_str_t jn_cut_841[] = {{"", 0}, {"bab ", 4}, {"b", 1}};
static const jn_str_t jn_cut_842[] = {{"", 0}, {"bxy", 3}, {"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_843[] = {{"", 0}, {"bxyabbba b", 10}};
static const jn_str_t jn_cut_844[] = {{"", 0}, {"bxybaxy", 7}};
static const jn_str_t jn_cut_845[] = {{"", 0}, {"xy", 2}, {"", 0}, {"a", 1}};
static const jn_str_t jn_cut_846[] = {{"", 0}, {"xy b b", 6}, {"", 0}, {" ", 1}, {"", 0}};
static const jn_str_t jn_cut_847[] = {{"", 0}, {"xyb b", 5}, {"xy a", 4}};
static const jn_str_t jn_cut_848[] = {{" ", 1}};
static const jn_str_t jn_cut_849[] = {{" ", 1}, {"", 0}};
static const jn_str_t jn_cut_850[] = {{" ", 1}, {"ab ab ", 6}};
static const jn_str_t jn_cut_851[] = {{" ", 1}, {"b", 1}};
static const jn_str_t jn_cut_852[] = {{" ", 1}, {"bb   axy", 8}};
static const jn_str_t jn_cut_853[] = {{" ", 1}, {"xy", 2}, {" ", 1}, {" xy", 3}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_854[] = {{"  a", 3}};
static const jn_str_t jn_cut_855[] = {{"  abxyxyabb", 11}};
static const jn_str_t jn_cut_856[] = {{" a", 2}, {"", 0}};
static const jn_str_t jn_cut_857[] = {{" a", 2}, {"", 0}, {" b b", 4}};
static const jn_str_t jn_cut_858[] = {{" a", 2}, {"a", 1}};
static const jn_str_t jn_cut_859[] = {{" a ", 3}};
static const jn_str_t jn_cut_860[] = {{" a a a  ", 8}};
static const jn_str_t jn_cut_861[] = {{" a xy ", 6}, {"xy", 2}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_862[] = {{" aa", 3}, {" b", 2}, {"", 0}};
static const jn_str_t jn_cut_863[] = {{" abbxy", 6}, {"", 0}, {" ", 1}};
static const jn_str_t jn_cut_864[] = {{" axy", 4}};
static const jn_str_t jn_cut_865[] = {{" b ", 3}};
static const jn_str_t jn_cut_866[] = {{" ba", 3}};
static const jn_str_t jn_cut_867[] = {{" baxy xyb", 9}};
static const jn_str_t jn_cut_868[] = {{" bxyaaa a", 9}};
static const jn_str_t jn_cut_869[] = {{" xybb", 5}};
static const jn_str_t jn_cut_870[] = {{" xybb", 5}, {"xyxy", 4}};
static const jn_str_t jn_cut_871[] = {{"a", 1}, {"", 0}, {"", 0}, {"abbab", 5}};
static const jn_str_t jn_cut_872[] = {{"a", 1}, {"", 0}, {"bab ", 4}};
static const jn_str_t jn_cut_873[] = {{"a", 1}, {" ", 1}};
static const jn_str_t jn_cut_874[] = {{"a", 1}, {" xy", 3}};
static const jn_str_t jn_cut_875[] = {{"a  xy ", 6}};
static const jn_str_t jn_cut_876[] = {{"a  xyxya", 8}, {" b", 2}};
static const jn_str_t jn_cut_877[] = {{"aa", 2}};
static const jn_str_t jn_cut_878[] = {{"aa ", 3}, {"b", 1}};
static const jn_str_t jn_cut_879[] = {{"aa  ", 4}, {"xyxy", 4}};
static const jn_str_t jn_cut_880[] = {{"aaa", 3}};
static const jn_str_t jn_cut_881[] = {{"aaa bxya", 8}};
static const jn_str_t jn_cut_882[] = {{"aaaa ", 5}};
static const jn_str_t jn_cut_883[] = {{"aab ", 4}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_884[] = {{"aaxy", 4}, {"xyxya", 5}, {"b ", 2}};
static const jn_str_t jn_cut_885[] = {{"aaxy bxy", 8}};
static const jn_str_t jn_cut_886[] = {{"ab ", 3}};
static const jn_str_t jn_cut_887[] = {{"abab", 4}, {"xy xy a", 7}};
static const jn_str_t jn_cut_888[] = {{"abbxy", 5}};
static const jn_str_t jn_cut_889[] = {{"abxy", 4}};
static const jn_str_t jn_cut_890[] = {{"abxyxyb", 7}};
static const jn_str_t jn_cut_891[] = {{"axy", 3}, {"bb  bbxy", 8}};
static const jn_str_t jn_cut_892[] = {{"axy ", 4}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_893[] = {{"b", 1}};
static const jn_str_t jn_cut_894[] = {{"b", 1}, {" aaa", 4}};
static const jn_str_t jn_cut_895[] = {{"b", 1}, {" xy", 3}};
static const jn_str_t jn_cut_896[] = {{"b", 1}, {"xyxy", 4}};
static const jn_str_t jn_cut_897[] = {{"b ", 2}};
static const jn_str_t jn_cut_898[] = {{"b   a ", 6}, {"", 0}};
static const jn_str_t jn_cut_899[] = {{"b xy xy ", 8}, {" ", 1}, {"xy", 2}};
static const jn_str_t jn_cut_900[] = {{"b xya xy", 8}};
static const jn_str_t jn_cut_901[] = {{"ba  bxyxy", 9}, {"ba", 2}};
static const jn_str_t jn_cut_902[] = {{"baaaxy", 6}};
static const jn_str_t jn_cut_903[] = {{"baxy", 4}};
static const jn_str_t jn_cut_904[] = {{"baxya b", 7}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_905[] = {{"baxyb", 5}};
static const jn_str_t jn_cut_906[] = {{"bb", 2}};
static const jn_str_t jn_cut_907[] = {{"bb", 2}, {"xy", 2}};
static const jn_str_t jn_cut_908[] = {{"bb xy    xyb", 12}};
static const jn_str_t jn_cut_909[] = {{"bba a", 5}, {"b", 1}, {"bxy", 3}};
static const jn_str_t jn_cut_910[] = {{"bxy", 3}};
static const jn_str_t jn_cut_911[] = {{"bxy", 3}, {"", 0}, {"xy", 2}, {"xy", 2}};
static const jn_str_t jn_cut_912[] = {{"bxy", 3}, {"baxya", 5}};
static const jn_str_t jn_cut_913[] = {{"bxy axyb ", 9}, {"", 0}};
static const jn_str_t jn_cut_914[] = {{"bxyaaxy b", 9}, {"", 0}};
static const jn_str_t jn_cut_915[] = {{"bxyb", 4}, {"", 0}};
static const jn_str_t jn_cut_916[] = {{"bxyxyxyxy", 9}};
static const jn_str_t jn_cut_917[] = {{"xy", 2}};
static const jn_str_t jn_cut_918[] = {{"xy", 2}, {"", 0}};
static const jn_str_t jn_cut_919[] = {{"xy", 2}, {"", 0}, {"ba    ", 6}};
static const jn_str_t jn_cut_920[] = {{"xy", 2}, {" bxy", 4}, {"b  ", 3}};
static const jn_str_t jn_cut_921[] = {{"xy ", 3}};
static const jn_str_t jn_cut_922[] = {{"xy ", 3}, {"b a", 3}};
static const jn_str_t jn_cut_923[] = {{"xy  xyxy b", 10}, {" ", 1}};
static const jn_str_t jn_cut_924[] = {{"xy a", 4}, {"   ", 3}, {"bxy", 3}};
static const jn_str_t jn_cut_925[] = {{"xy aa", 5}, {"", 0}};
static const jn_str_t jn_cut_926[] = {{"xy xy", 5}, {" ", 1}, {"xy bb", 5}};
static const jn_str_t jn_cut_927[] = {{"xya ", 4}, {"", 0}, {"a", 1}};
static const jn_str_t jn_cut_928[] = {{"xyaab", 5}};
static const jn_str_t jn_cut_929[] = {{"xyaabb", 6}};
static const jn_str_t jn_cut_930[] = {{"xyaaxy baxy", 11}};
static const jn_str_t jn_cut_931[] = {{"xyb", 3}, {"bab", 3}, {" ab", 3}};
static const jn_str_t jn_cut_932[] = {{"xyba ", 5}, {"b", 1}};
static const jn_str_t jn_cut_933[] = {{"xybaaxybxyxya", 13}};
static const jn_str_t jn_cut_934[] = {{"xybbxyxy", 8}, {" xya", 4}, {"", 0}};
static const jn_str_t jn_cut_935[] = {{"xyxy", 4}};
static const jn_str_t jn_cut_936[] = {{"xyxy", 4}, {" ", 1}};
static const jn_str_t jn_cut_937[] = {{"xyxy", 4}, {" xy  ", 5}};
static const jn_str_t jn_cut_938[] = {{"xyxy", 4}, {"b a", 3}};
static const jn_str_t jn_cut_939[] = {{"xyxya", 5}};
static const jn_str_t jn_cut_941[] = {{"", 0}, {"", 0}};
static const jn_str_t jn_cut_942[] = {{"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_943[] = {{"", 0}, {"", 0}};
static const jn_str_t jn_cut_944[] = {{"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_945[] = {{"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_946[] = {{"", 0}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_947[] = {{"", 0}, {"", 0}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_948[] = {{" ", 1}};
static const jn_str_t jn_cut_949[] = {{"  ", 2}};
static const jn_str_t jn_cut_950[] = {{"   ", 3}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"c", 1}, {"b ", 2}};
static const jn_str_t jn_cut_951[] = {{"  ", 2}, {" b", 2}};
static const jn_str_t jn_cut_952[] = {{"  ", 2}, {"", 0}, {" ", 1}, {"b", 1}, {"", 0}, {"", 0}, {"c", 1}, {"", 0}};
static const jn_str_t jn_cut_953[] = {{"  ", 2}, {"", 0}, {"c bbcc", 6}};
static const jn_str_t jn_cut_954[] = {{" ", 1}, {"", 0}};
static const jn_str_t jn_cut_955[] = {{" ", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_956[] = {{" ", 1}, {"", 0}, {" ", 1}, {"", 0}, {" ", 1}, {"b", 1}};
static const jn_str_t jn_cut_957[] = {{" ", 1}, {"", 0}, {" ", 1}, {"", 0}, {"", 0}, {"bb ", 3}};
static const jn_str_t jn_cut_958[] = {{" ", 1}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_959[] = {{" ", 1}, {"", 0}, {"", 0}, {"", 0}, {" ", 1}, {"", 0}, {"  ", 2}, {"", 0}};
static const jn_str_t jn_cut_960[] = {{" ", 1}, {"", 0}, {"", 0}, {"", 0}, {"cc", 2}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_961[] = {{" ", 1}, {"", 0}, {"b ", 2}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_962[] = {{" ", 1}, {"", 0}, {"c", 1}};
static const jn_str_t jn_cut_963[] = {{" ", 1}, {"bb ", 3}, {"  ", 2}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_964[] = {{" ", 1}, {"bb", 2}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_965[] = {{" b", 2}, {" b", 2}, {" c ", 3}};
static const jn_str_t jn_cut_966[] = {{" b", 2}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_967[] = {{" b", 2}, {"", 0}, {"", 0}, {" ", 1}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_968[] = {{" b", 2}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_969[] = {{" b", 2}, {"", 0}, {"", 0}, {"", 0}, {"b", 1}, {"", 0}, {"", 0}, {"c ", 2}};
static const jn_str_t jn_cut_970[] = {{" b", 2}, {"", 0}, {"b", 1}, {"", 0}, {" ", 1}};
static const jn_str_t jn_cut_971[] = {{" bc", 3}};
static const jn_str_t jn_cut_972[] = {{" bcb", 4}};
static const jn_str_t jn_cut_973[] = {{" c c", 4}};
static const jn_str_t jn_cut_974[] = {{"", 0}, {"   b", 4}};
static const jn_str_t jn_cut_975[] = {{"", 0}, {"  ", 2}, {"c", 1}};
static const jn_str_t jn_cut_976[] = {{"", 0}, {" b  bbc ", 8}};
static const jn_str_t jn_cut_977[] = {{"", 0}, {"", 0}, {" ", 1}, {"", 0}};
static const jn_str_t jn_cut_978[] = {{"", 0}, {"", 0}, {" ", 1}, {"", 0}, {"c", 1}, {"cc ", 3}};
static const jn_str_t jn_cut_979[] = {{"", 0}, {"", 0}, {" b ccb ", 7}, {" ", 1}};
static const jn_str_t jn_cut_980[] = {{"", 0}, {"", 0}, {" b", 2}, {"", 0}, {"", 0}, {"  ", 2}};
static const jn_str_t jn_cut_981[] = {{"", 0}, {"", 0}, {"", 0}, {" ", 1}, {"", 0}, {"", 0}, {"", 0}, {" b", 2}};
static const jn_str_t jn_cut_982[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_983[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {" bcb", 4}};
static const jn_str_t jn_cut_984[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {" cbc", 4}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_985[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {" ", 1}, {"c", 1}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_986[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {" c", 2}, {"", 0}, {"c", 1}, {"", 0}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_987[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"c", 1}, {"", 0}, {"bb", 2}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_988[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"bc", 2}, {"b", 1}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_989[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"cb", 2}};
static const jn_str_t jn_cut_990[] = {{"", 0}, {"", 0}, {"", 0}, {"b ", 2}};
static const jn_str_t jn_cut_991[] = {{"", 0}, {"", 0}, {"", 0}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_992[] = {{"", 0}, {"", 0}, {"", 0}, {"bb ", 3}, {"", 0}, {" ", 1}};
static const jn_str_t jn_cut_993[] = {{"", 0}, {"", 0}, {"", 0}, {"c", 1}, {" c c", 4}, {"b", 1}};
static const jn_str_t jn_cut_994[] = {{"", 0}, {"", 0}, {"", 0}, {"cb", 2}};
static const jn_str_t jn_cut_995[] = {{"", 0}, {"", 0}, {"", 0}, {"cbb", 3}, {"", 0}, {"c", 1}};
static const jn_str_t jn_cut_996[] = {{"", 0}, {"", 0}, {"b ", 2}, {"", 0}, {"bb", 2}, {"", 0}, {" ", 1}};
static const jn_str_t jn_cut_997[] = {{"", 0}, {"", 0}, {"b c", 3}};
static const jn_str_t jn_cut_998[] = {{"", 0}, {"", 0}, {"b", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_999[] = {{"", 0}, {"", 0}, {"bbc ", 4}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1000[] = {{"", 0}, {"", 0}, {"bbcc ", 5}};
static const jn_str_t jn_cut_1001[] = {{"", 0}, {"", 0}, {"bc c", 4}, {"b", 1}};
static const jn_str_t jn_cut_1002[] = {{"", 0}, {"", 0}, {"c", 1}, {"cccb", 4}};
static const jn_str_t jn_cut_1003[] = {{"", 0}, {"", 0}, {"ccb", 3}, {"b c ", 4}};
static const jn_str_t jn_cut_1004[] = {{"", 0}, {"b", 1}};
static const jn_str_t jn_cut_1005[] = {{"", 0}, {"b", 1}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {" c", 2}};
static const jn_str_t jn_cut_1006[] = {{"", 0}, {"b", 1}, {"b", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1007[] = {{"", 0}, {"bb ", 3}};
static const jn_str_t jn_cut_1008[] = {{"", 0}, {"c", 1}};
static const jn_str_t jn_cut_1009[] = {{"", 0}, {"c", 1}, {" b", 2}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1010[] = {{"", 0}, {"c", 1}, {"", 0}, {" ", 1}, {"", 0}};
static const jn_str_t jn_cut_1011[] = {{"", 0}, {"c", 1}, {"", 0}, {" bc", 3}, {"", 0}, {"", 0}, {"", 0}, {" ", 1}};
static const jn_str_t jn_cut_1012[] = {{"", 0}, {"cb ", 3}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1013[] = {{"", 0}, {"cb", 2}, {"c  c", 4}};
static const jn_str_t jn_cut_1014[] = {{"b", 1}};
static const jn_str_t jn_cut_1015[] = {{"b ", 2}, {"", 0}, {"bc", 2}, {"", 0}, {"", 0}, {" c", 2}};
static const jn_str_t jn_cut_1016[] = {{"b b", 3}, {"", 0}, {"bc", 2}, {"", 0}, {" ", 1}, {"", 0}};
static const jn_str_t jn_cut_1017[] = {{"b bb", 4}};
static const jn_str_t jn_cut_1018[] = {{"b bb", 4}, {"", 0}, {"b ", 2}, {"", 0}, {" ", 1}};
static const jn_str_t jn_cut_1019[] = {{"b bc", 4}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1020[] = {{"b", 1}, {"", 0}};
static const jn_str_t jn_cut_1021[] = {{"b", 1}, {" bc", 3}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"bc", 2}};
static const jn_str_t jn_cut_1022[] = {{"b", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1023[] = {{"b", 1}, {"", 0}, {"  b", 3}};
static const jn_str_t jn_cut_1024[] = {{"b", 1}, {"", 0}, {"", 0}, {"cb", 2}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1025[] = {{"b", 1}, {"", 0}, {"c ", 2}};
static const jn_str_t jn_cut_1026[] = {{"b", 1}, {"", 0}, {"c cbb", 5}, {"", 0}, {" ", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1027[] = {{"b", 1}, {"", 0}, {"c", 1}, {"c", 1}, {"", 0}};
static const jn_str_t jn_cut_1028[] = {{"b", 1}, {"c", 1}, {"  bc", 4}, {"", 0}};
static const jn_str_t jn_cut_1029[] = {{"bb", 2}, {"", 0}};
static const jn_str_t jn_cut_1030[] = {{"bb", 2}, {"", 0}, {"", 0}, {"", 0}, {"b", 1}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_1031[] = {{"bb", 2}, {"", 0}, {"c", 1}, {"", 0}};
static const jn_str_t jn_cut_1032[] = {{"bbb", 3}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1033[] = {{"bbbb", 4}, {"c", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1034[] = {{"bc b", 4}, {"  ", 2}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1035[] = {{"bc c", 4}, {"  c", 3}};
static const jn_str_t jn_cut_1036[] = {{"bc", 2}, {"", 0}, {"", 0}, {"", 0}, {"c", 1}, {"", 0}, {" b", 2}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1037[] = {{"bcbb ", 5}};
static const jn_str_t jn_cut_1038[] = {{"bcc", 3}};
static const jn_str_t jn_cut_1039[] = {{"c", 1}};
static const jn_str_t jn_cut_1040[] = {{"c ", 2}};
static const jn_str_t jn_cut_1041[] = {{"c ", 2}, {"", 0}, {" ", 1}};
static const jn_str_t jn_cut_1042[] = {{"c ", 2}, {"", 0}, {"", 0}, {"", 0}, {"ccbbc", 5}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1043[] = {{"c b", 3}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1044[] = {{"c cc", 4}};
static const jn_str_t jn_cut_1045[] = {{"c", 1}, {" ", 1}, {"", 0}};
static const jn_str_t jn_cut_1046[] = {{"c", 1}, {" ", 1}, {"", 0}, {"", 0}, {"c ", 2}, {"", 0}, {"b ", 2}};
static const jn_str_t jn_cut_1047[] = {{"c", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1048[] = {{"c", 1}, {"", 0}, {" ", 1}, {"c", 1}};
static const jn_str_t jn_cut_1049[] = {{"c", 1}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1050[] = {{"c", 1}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1051[] = {{"c", 1}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_1052[] = {{"c", 1}, {"", 0}, {"b ", 2}, {"", 0}, {"", 0}, {" ", 1}};
static const jn_str_t jn_cut_1053[] = {{"c", 1}, {"", 0}, {"bb", 2}, {"", 0}, {" b", 2}};
static const jn_str_t jn_cut_1054[] = {{"c", 1}, {"", 0}, {"c b ", 4}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1055[] = {{"c", 1}, {"", 0}, {"cb  ", 4}};
static const jn_str_t jn_cut_1056[] = {{"cb ", 3}, {"bb", 2}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1057[] = {{"cb", 2}, {"", 0}, {"", 0}, {"", 0}, {"  ", 2}};
static const jn_str_t jn_cut_1058[] = {{"cb", 2}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1059[] = {{"cbb", 3}};
static const jn_str_t jn_cut_1060[] = {{"cbc ", 4}, {"", 0}, {"c", 1}, {"", 0}, {"", 0}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_1061[] = {{"cc", 2}};
static const jn_str_t jn_cut_1062[] = {{"ccb", 3}, {"b", 1}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1063[] = {{"ccbbb", 5}, {"", 0}, {"c", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1064[] = {{"ccccb", 5}};
static const jn_str_t jn_cut_1066[] = {{"", 0}, {"", 0}};
static const jn_str_t jn_cut_1067[] = {{"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1068[] = {{"a", 1}};
static const jn_str_t jn_cut_1069[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_1070[] = {{"", 0}, {"a", 1}};
static const jn_str_t jn_cut_1071[] = {{"a", 1}, {"b", 1}};
static const jn_str_t jn_cut_1072[] = {{"a", 1}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_1073[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1074[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {" ", 1}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1075[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"a", 1}};
static const jn_str_t jn_cut_1076[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"b", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1077[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"b", 1}, {"", 0}, {"aa", 2}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1078[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {" a", 2}};
static const jn_str_t jn_cut_1079[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {" b", 2}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1080[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {" bb", 3}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1081[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"a", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1082[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"a ", 2}, {"", 0}, {" ", 1}};
static const jn_str_t jn_cut_1083[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"b a", 3}};
static const jn_str_t jn_cut_1084[] = {{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"bb", 2}, {"", 0}, {"b", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1085[] = {{"", 0}, {"", 0}, {"", 0}, {" ", 1}};
static const jn_str_t jn_cut_1086[] = {{"", 0}, {"", 0}, {"", 0}, {" ", 1}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1087[] = {{"", 0}, {"", 0}, {"", 0}, {" a", 2}, {"a", 1}, {"a", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1088[] = {{"", 0}, {"", 0}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_1089[] = {{"", 0}, {"", 0}, {"", 0}, {"b", 1}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1090[] = {{"", 0}, {"", 0}, {"", 0}, {"b", 1}, {"", 0}, {" ", 1}, {"", 0}};
static const jn_str_t jn_cut_1091[] = {{"", 0}, {"", 0}, {"", 0}, {"ba b ", 5}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1092[] = {{"", 0}, {"", 0}, {" ", 1}, {"", 0}, {"", 0}, {" ", 1}};
static const jn_str_t jn_cut_1093[] = {{"", 0}, {"", 0}, {" ", 1}, {"", 0}, {"  ", 2}};
static const jn_str_t jn_cut_1094[] = {{"", 0}, {"", 0}, {" ", 1}, {"a", 1}};
static const jn_str_t jn_cut_1095[] = {{"", 0}, {"", 0}, {" a", 2}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1096[] = {{"", 0}, {"", 0}, {" aa", 3}, {" baa", 4}};
static const jn_str_t jn_cut_1097[] = {{"", 0}, {"", 0}, {" b", 2}};
static const jn_str_t jn_cut_1098[] = {{"", 0}, {"", 0}, {" b   b", 6}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1099[] = {{"", 0}, {"", 0}, {" b b", 4}};
static const jn_str_t jn_cut_1100[] = {{"", 0}, {"", 0}, {" b b", 4}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1101[] = {{"", 0}, {"", 0}, {" bab", 4}, {"", 0}, {"ab", 2}};
static const jn_str_t jn_cut_1102[] = {{"", 0}, {"", 0}, {"a", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1103[] = {{"", 0}, {"", 0}, {"a", 1}, {"", 0}, {" ", 1}};
static const jn_str_t jn_cut_1104[] = {{"", 0}, {"", 0}, {"a aa", 4}};
static const jn_str_t jn_cut_1105[] = {{"", 0}, {"", 0}, {"aa", 2}, {"", 0}, {"aa ", 3}, {"", 0}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_1106[] = {{"", 0}, {"", 0}, {"ab", 2}, {" ", 1}, {"", 0}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_1107[] = {{"", 0}, {"", 0}, {"abbbaaa", 7}};
static const jn_str_t jn_cut_1108[] = {{"", 0}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_1109[] = {{"", 0}, {"", 0}, {"b", 1}, {"", 0}, {"b", 1}, {"b ", 2}};
static const jn_str_t jn_cut_1110[] = {{"", 0}, {"", 0}, {"b", 1}, {"b", 1}, {"", 0}, {" a", 2}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1111[] = {{"", 0}, {"", 0}, {"b a", 3}, {"", 0}, {"", 0}, {" ", 1}};
static const jn_str_t jn_cut_1112[] = {{"", 0}, {"", 0}, {"bb", 2}, {"", 0}, {"  b", 3}, {" ", 1}};
static const jn_str_t jn_cut_1113[] = {{"", 0}, {" ", 1}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1114[] = {{"", 0}, {" ", 1}, {"", 0}, {"", 0}, {"b a", 3}, {"b", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1115[] = {{"", 0}, {" ", 1}, {"", 0}, {" b", 2}};
static const jn_str_t jn_cut_1116[] = {{"", 0}, {"  ", 2}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"bb", 2}};
static const jn_str_t jn_cut_1117[] = {{"", 0}, {"  ", 2}, {" ", 1}};
static const jn_str_t jn_cut_1118[] = {{"", 0}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_1119[] = {{"", 0}, {"a", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1120[] = {{"", 0}, {"a bb", 4}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1121[] = {{"", 0}, {"aa ", 3}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1122[] = {{" ", 1}};
static const jn_str_t jn_cut_1123[] = {{" ", 1}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1124[] = {{" ", 1}, {"", 0}, {"", 0}, {"", 0}, {" ", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1125[] = {{" ", 1}, {"", 0}, {"", 0}, {"", 0}, {"ab a ", 5}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1126[] = {{" ", 1}, {"", 0}, {"", 0}, {" a", 2}, {"", 0}, {"", 0}, {"b b", 3}};
static const jn_str_t jn_cut_1127[] = {{" ", 1}, {"", 0}, {"", 0}, {"a a", 3}};
static const jn_str_t jn_cut_1128[] = {{" ", 1}, {"", 0}, {"", 0}, {"aba", 3}};
static const jn_str_t jn_cut_1129[] = {{" ", 1}, {"", 0}, {"  ", 2}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1130[] = {{" ", 1}, {"", 0}, {" b", 2}};
static const jn_str_t jn_cut_1131[] = {{" ", 1}, {"", 0}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_1132[] = {{" ", 1}, {"", 0}, {"b  ", 3}};
static const jn_str_t jn_cut_1133[] = {{" ", 1}, {" aa", 3}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1134[] = {{"  ", 2}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1135[] = {{"  ", 2}, {"", 0}, {"", 0}, {" ", 1}};
static const jn_str_t jn_cut_1136[] = {{"   ba ", 6}, {"aa", 2}};
static const jn_str_t jn_cut_1137[] = {{"  ba", 4}, {"", 0}, {"  ", 2}, {"a", 1}};
static const jn_str_t jn_cut_1138[] = {{" a", 2}};
static const jn_str_t jn_cut_1139[] = {{" a b a", 6}, {"b", 1}};
static const jn_str_t jn_cut_1140[] = {{" aba bb", 7}, {"", 0}, {" ", 1}};
static const jn_str_t jn_cut_1141[] = {{" b", 2}};
static const jn_str_t jn_cut_1142[] = {{" b", 2}, {"", 0}};
static const jn_str_t jn_cut_1143[] = {{" ba ", 4}};
static const jn_str_t jn_cut_1144[] = {{"a", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1145[] = {{"a", 1}, {"", 0}, {"", 0}, {"b", 1}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {" b", 2}};
static const jn_str_t jn_cut_1146[] = {{"a", 1}, {"", 0}, {" ", 1}, {"", 0}, {"aa", 2}, {"b", 1}, {"a", 1}};
static const jn_str_t jn_cut_1147[] = {{"a", 1}, {"", 0}, {" b", 2}, {" aab", 4}};
static const jn_str_t jn_cut_1148[] = {{"a", 1}, {"", 0}, {"a ", 2}};
static const jn_str_t jn_cut_1149[] = {{"a", 1}, {" ", 1}};
static const jn_str_t jn_cut_1150[] = {{"a", 1}, {"a", 1}};
static const jn_str_t jn_cut_1151[] = {{"a", 1}, {"aa ", 3}, {"", 0}, {"b ", 2}, {"", 0}};
static const jn_str_t jn_cut_1152[] = {{"a", 1}, {"bab", 3}};
static const jn_str_t jn_cut_1153[] = {{"a ", 2}, {"", 0}, {"a", 1}, {"", 0}, {"aa", 2}};
static const jn_str_t jn_cut_1154[] = {{"a a ", 4}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1155[] = {{"a a aa", 6}, {" b ", 3}};
static const jn_str_t jn_cut_1156[] = {{"a bba", 5}};
static const jn_str_t jn_cut_1157[] = {{"aaa   aa", 8}};
static const jn_str_t jn_cut_1158[] = {{"aaaab ", 6}};
static const jn_str_t jn_cut_1159[] = {{"ab", 2}, {"", 0}, {"b", 1}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1160[] = {{"ab", 2}, {"b", 1}, {"", 0}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_1161[] = {{"aba ", 4}};
static const jn_str_t jn_cut_1162[] = {{"b", 1}};
static const jn_str_t jn_cut_1163[] = {{"b", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1164[] = {{"b", 1}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"a", 1}};
static const jn_str_t jn_cut_1165[] = {{"b", 1}, {"", 0}, {"   ", 3}, {"aa  ", 4}};
static const jn_str_t jn_cut_1166[] = {{"b", 1}, {"", 0}, {"ab", 2}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1167[] = {{"b", 1}, {"", 0}, {"b", 1}, {"aba", 3}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1168[] = {{"b", 1}, {" a", 2}, {"", 0}, {"", 0}, {"", 0}, {"bb", 2}};
static const jn_str_t jn_cut_1169[] = {{"b", 1}, {"a a", 3}, {"", 0}, {"bba", 3}};
static const jn_str_t jn_cut_1170[] = {{"b", 1}, {"bbb", 3}};
static const jn_str_t jn_cut_1171[] = {{"b  ", 3}, {"", 0}, {"", 0}, {"  a", 3}};
static const jn_str_t jn_cut_1172[] = {{"b a", 3}};
static const jn_str_t jn_cut_1173[] = {{"b a  ", 5}, {"b", 1}};
static const jn_str_t jn_cut_1174[] = {{"b aa ", 5}};
static const jn_str_t jn_cut_1175[] = {{"ba", 2}, {"", 0}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1176[] = {{"ba", 2}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_1177[] = {{"ba", 2}, {" bbbb", 5}};
static const jn_str_t jn_cut_1178[] = {{"ba", 2}, {"a ", 2}};
static const jn_str_t jn_cut_1179[] = {{"ba", 2}, {"bb ", 3}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1180[] = {{"ba aa", 5}, {"", 0}, {" ", 1}, {"", 0}, {"", 0}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_1181[] = {{"bb", 2}, {"", 0}, {"a", 1}};
static const jn_str_t jn_cut_1182[] = {{"bb", 2}, {"a ", 2}};
static const jn_str_t jn_cut_1183[] = {{"bb ab", 5}};
static const jn_str_t jn_cut_1184[] = {{"bba", 3}};
static const jn_str_t jn_cut_1185[] = {{"bbaa", 4}, {"", 0}, {"b", 1}, {"", 0}, {" ", 1}};
static const jn_str_t jn_cut_1186[] = {{"bbaa ", 5}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1188[] = {{"", 0}, {"", 0}};
static const jn_str_t jn_cut_1189[] = {{"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1190[] = {{"a", 1}};
static const jn_str_t jn_cut_1191[] = {{"a", 1}, {"", 0}};
static const jn_str_t jn_cut_1192[] = {{"", 0}, {"a", 1}};
static const jn_str_t jn_cut_1193[] = {{"a", 1}, {"b", 1}};
static const jn_str_t jn_cut_1194[] = {{"a", 1}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_1195[] = {{" ", 1}};
static const jn_str_t jn_cut_1196[] = {{"   ", 3}};
static const jn_str_t jn_cut_1197[] = {{"   b ", 5}, {"aa", 2}, {" ", 1}};
static const jn_str_t jn_cut_1198[] = {{"  aba", 5}};
static const jn_str_t jn_cut_1199[] = {{"  ", 2}, {"ab a", 4}};
static const jn_str_t jn_cut_1200[] = {{" a", 2}};
static const jn_str_t jn_cut_1201[] = {{" a  ", 4}};
static const jn_str_t jn_cut_1202[] = {{" a b a", 6}};
static const jn_str_t jn_cut_1203[] = {{" aaa ", 5}, {"", 0}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_1204[] = {{" aaba", 5}, {"a ", 2}};
static const jn_str_t jn_cut_1205[] = {{" aa", 3}, {"bab   ", 6}};
static const jn_str_t jn_cut_1206[] = {{" ab aba ", 8}};
static const jn_str_t jn_cut_1207[] = {{" a", 2}, {" a", 2}, {"", 0}};
static const jn_str_t jn_cut_1208[] = {{" a", 2}, {" b", 2}};
static const jn_str_t jn_cut_1209[] = {{" a", 2}, {"a", 1}, {"", 0}, {"a a", 3}, {"", 0}};
static const jn_str_t jn_cut_1210[] = {{" a", 2}, {"ba", 2}, {"abb", 3}};
static const jn_str_t jn_cut_1211[] = {{" a", 2}, {"bb", 2}};
static const jn_str_t jn_cut_1212[] = {{" a", 2}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1213[] = {{" b b", 4}, {"b", 1}};
static const jn_str_t jn_cut_1214[] = {{" b ", 3}, {"", 0}, {"", 0}, {"a ", 2}};
static const jn_str_t jn_cut_1215[] = {{" ba", 3}};
static const jn_str_t jn_cut_1216[] = {{" ba", 3}, {" ", 1}};
static const jn_str_t jn_cut_1217[] = {{" bba", 4}};
static const jn_str_t jn_cut_1218[] = {{" bbbb", 5}, {"", 0}};
static const jn_str_t jn_cut_1219[] = {{" b", 2}, {"", 0}, {"a ab", 4}};
static const jn_str_t jn_cut_1220[] = {{" b", 2}, {"", 0}, {"baa", 3}};
static const jn_str_t jn_cut_1221[] = {{" ", 1}, {" a", 2}, {"", 0}};
static const jn_str_t jn_cut_1222[] = {{" ", 1}, {" a", 2}, {"", 0}, {"bbb", 3}};
static const jn_str_t jn_cut_1223[] = {{" ", 1}, {" ", 1}, {" ", 1}};
static const jn_str_t jn_cut_1224[] = {{" ", 1}, {"ab", 2}};
static const jn_str_t jn_cut_1225[] = {{" ", 1}, {"b ", 2}};
static const jn_str_t jn_cut_1226[] = {{" ", 1}, {"b b baa", 7}};
static const jn_str_t jn_cut_1227[] = {{" ", 1}, {"bab ", 4}, {"b", 1}};
static const jn_str_t jn_cut_1228[] = {{"a  a", 4}, {"", 0}};
static const jn_str_t jn_cut_1229[] = {{"a aa", 4}};
static const jn_str_t jn_cut_1230[] = {{"a a", 3}, {" a", 2}, {"", 0}};
static const jn_str_t jn_cut_1231[] = {{"a a", 3}, {"b a", 3}, {"", 0}};
static const jn_str_t jn_cut_1232[] = {{"a baaab", 7}};
static const jn_str_t jn_cut_1233[] = {{"a baa", 5}, {"a  b", 4}};
static const jn_str_t jn_cut_1234[] = {{"a bab", 5}, {"", 0}};
static const jn_str_t jn_cut_1235[] = {{"a bbaa", 6}, {"", 0}};
static const jn_str_t jn_cut_1236[] = {{"a b", 3}, {"", 0}, {" ", 1}, {"a", 1}, {"a", 1}};
static const jn_str_t jn_cut_1237[] = {{"a ", 2}, {"", 0}};
static const jn_str_t jn_cut_1238[] = {{"aa ", 3}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_1239[] = {{"aaa  a", 6}};
static const jn_str_t jn_cut_1240[] = {{"aaa ba", 6}, {"", 0}};
static const jn_str_t jn_cut_1241[] = {{"aaa", 3}, {"bb", 2}, {"aa", 2}};
static const jn_str_t jn_cut_1242[] = {{"aaa", 3}, {"", 0}, {"aaa", 3}};
static const jn_str_t jn_cut_1243[] = {{"ab", 2}};
static const jn_str_t jn_cut_1244[] = {{"aba", 3}};
static const jn_str_t jn_cut_1245[] = {{"aba  ", 5}, {" ", 1}, {"a", 1}};
static const jn_str_t jn_cut_1246[] = {{"aba", 3}, {"ba", 2}};
static const jn_str_t jn_cut_1247[] = {{"abba a ", 7}};
static const jn_str_t jn_cut_1248[] = {{"abbab", 5}};
static const jn_str_t jn_cut_1249[] = {{"abb", 3}, {"aa", 2}};
static const jn_str_t jn_cut_1250[] = {{"ab", 2}, {"", 0}, {" bb", 3}};
static const jn_str_t jn_cut_1251[] = {{"a", 1}, {"a ab ", 5}};
static const jn_str_t jn_cut_1252[] = {{"a", 1}, {"aa", 2}, {"a", 1}, {"b ", 2}};
static const jn_str_t jn_cut_1253[] = {{"a", 1}, {"bb", 2}, {"ba", 2}};
static const jn_str_t jn_cut_1254[] = {{"a", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1255[] = {{"a", 1}, {"", 0}, {"aa b", 4}};
static const jn_str_t jn_cut_1256[] = {{"b", 1}};
static const jn_str_t jn_cut_1257[] = {{"b  b", 4}};
static const jn_str_t jn_cut_1258[] = {{"b aa b", 6}, {"", 0}};
static const jn_str_t jn_cut_1259[] = {{"b ab ab", 7}};
static const jn_str_t jn_cut_1260[] = {{"b abab", 6}, {" ", 1}, {"b", 1}};
static const jn_str_t jn_cut_1261[] = {{"b a", 3}, {" ", 1}, {"b", 1}};
static const jn_str_t jn_cut_1262[] = {{"b ba", 4}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1263[] = {{"b ", 2}, {" ", 1}, {" b", 2}, {"a", 1}};
static const jn_str_t jn_cut_1264[] = {{"b ", 2}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1265[] = {{"b ", 2}, {"", 0}, {"", 0}, {"b ", 2}};
static const jn_str_t jn_cut_1266[] = {{"ba", 2}};
static const jn_str_t jn_cut_1267[] = {{"ba ", 3}, {" ", 1}};
static const jn_str_t jn_cut_1268[] = {{"ba ", 3}, {"b", 1}};
static const jn_str_t jn_cut_1269[] = {{"baa", 3}};
static const jn_str_t jn_cut_1270[] = {{"baa ", 4}};
static const jn_str_t jn_cut_1271[] = {{"baa   ab b", 10}};
static const jn_str_t jn_cut_1272[] = {{"baa ", 4}, {"", 0}};
static const jn_str_t jn_cut_1273[] = {{"baaa ", 5}, {"ba  ", 4}};
static const jn_str_t jn_cut_1274[] = {{"babab", 5}, {"", 0}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1275[] = {{"ba", 2}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_1276[] = {{"bb bb", 5}, {" b", 2}, {"a", 1}};
static const jn_str_t jn_cut_1277[] = {{"bbab  ", 6}};
static const jn_str_t jn_cut_1278[] = {{"bba", 3}, {"", 0}};
static const jn_str_t jn_cut_1279[] = {{"bbb", 3}};
static const jn_str_t jn_cut_1280[] = {{"bb", 2}, {" b  aba", 7}};
static const jn_str_t jn_cut_1281[] = {{"bb", 2}, {"b", 1}, {" ", 1}, {"aaa", 3}};
static const jn_str_t jn_cut_1282[] = {{"b", 1}, {"", 0}};
static const jn_str_t jn_cut_1283[] = {{"b", 1}, {"  ", 2}, {"b", 1}};
static const jn_str_t jn_cut_1284[] = {{"b", 1}, {" b", 2}};
static const jn_str_t jn_cut_1285[] = {{"b", 1}, {" b b", 4}, {"", 0}, {"", 0}, {"b", 1}};
static const jn_str_t jn_cut_1286[] = {{"b", 1}, {"ababb", 5}, {"aa", 2}};
static const jn_str_t jn_cut_1287[] = {{"b", 1}, {"b aa", 4}};
static const jn_str_t jn_cut_1288[] = {{"b", 1}, {"babb", 4}, {"b", 1}};
static const jn_str_t jn_cut_1289[] = {{"b", 1}, {"", 0}, {"aa", 2}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_1290[] = {{"", 0}, {"  b  a  ", 8}};
static const jn_str_t jn_cut_1291[] = {{"", 0}, {" a bb", 5}, {"", 0}};
static const jn_str_t jn_cut_1292[] = {{"", 0}, {" ", 1}, {"", 0}};
static const jn_str_t jn_cut_1293[] = {{"", 0}, {"aa ba b a", 9}};
static const jn_str_t jn_cut_1294[] = {{"", 0}, {"abaa", 4}, {"", 0}, {"ab", 2}};
static const jn_str_t jn_cut_1295[] = {{"", 0}, {"abba", 4}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1296[] = {{"", 0}, {"ab", 2}, {"", 0}, {"b", 1}, {"b", 1}, {"", 0}};
static const jn_str_t jn_cut_1297[] = {{"", 0}, {"a", 1}, {"", 0}};
static const jn_str_t jn_cut_1298[] = {{"", 0}, {"a", 1}, {" ", 1}};
static const jn_str_t jn_cut_1299[] = {{"", 0}, {"a", 1}, {"", 0}, {" ", 1}};
static const jn_str_t jn_cut_1300[] = {{"", 0}, {"a", 1}, {"", 0}, {" ", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1301[] = {{"", 0}, {"b", 1}};
static const jn_str_t jn_cut_1302[] = {{"", 0}, {"baa", 3}, {" ", 1}, {" ", 1}, {"b", 1}};
static const jn_str_t jn_cut_1303[] = {{"", 0}, {"bab  ", 5}};
static const jn_str_t jn_cut_1304[] = {{"", 0}, {"ba", 2}, {"", 0}, {"", 0}, {" ", 1}};
static const jn_str_t jn_cut_1305[] = {{"", 0}, {"bba", 3}, {"a ", 2}};
static const jn_str_t jn_cut_1306[] = {{"", 0}, {"bbb", 3}, {"bb", 2}};
static const jn_str_t jn_cut_1307[] = {{"", 0}, {"b", 1}, {"", 0}, {"b", 1}, {"", 0}, {"", 0}};
static const jn_str_t jn_cut_1308[] = {{"", 0}, {"", 0}, {" b", 2}, {" a", 2}, {"", 0}};
static const jn_str_t jn_cut_1309[] = {{"", 0}, {"", 0}, {"a ", 2}, {"bba  ", 5}};
static const jn_str_t jn_cut_1310[] = {{"", 0}, {"", 0}, {"", 0}, {"ba", 2}, {"", 0}, {"ba", 2}};
static const jn_str_t jn_cut_1312[] = {{"a", 1}};
static const jn_str_t jn_cut_1313[] = {{" ", 1}};
static const jn_str_t jn_cut_1314[] = {{"a b", 3}};
static const jn_str_t jn_cut_1315[] = {{"a,b", 3}};
static const jn_str_t jn_cut_1316[] = {{"\011", 1}};
static const jn_str_t jn_cut_1317[] = {{"  x  ", 5}};
static const jn_str_t jn_cut_1318[] = {{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 300}};
static const jn_str_t jn_cut_1319[] = {{"a\012b", 3}};
static const jn_str_t jn_cut_1320[] = {{"\012", 1}};
static const jn_str_t jn_cut_1321[] = {{"a\012", 2}};
static const jn_str_t jn_cut_1322[] = {{"\012a", 2}};
static const jn_str_t jn_cut_1323[] = {{"a\012\012b", 4}};
static const jn_str_t jn_cut_1324[] = {{"a b\012c d", 7}};

static const jn_cut_case_t jn_cut_cases[] = {
    {-1, {"", 0}, 0, 0},
    {-1, {" ", 1}, 0, 0},
    {-1, {"  ", 2}, 0, 0},
    {-1, {"\011", 1}, 0, 0},
    {-1, {" \011 ", 3}, 0, 0},
    {-1, {"a", 1}, 1, jn_cut_5},
    {-1, {" a", 2}, 1, jn_cut_6},
    {-1, {"a ", 2}, 2, jn_cut_7},
    {-1, {" a ", 3}, 2, jn_cut_8},
    {-1, {"a b", 3}, 2, jn_cut_9},
    {-1, {"a  b", 4}, 2, jn_cut_10},
    {-1, {"a b ", 4}, 3, jn_cut_11},
    {-1, {"a\011b", 3}, 2, jn_cut_12},
    {-1, {"a \011 b", 5}, 2, jn_cut_13},
    {-1, {"  a  b  ", 8}, 3, jn_cut_14},
    {-1, {"a b c d e f g h", 15}, 8, jn_cut_15},
    {-1, {"\011a\011", 3}, 2, jn_cut_16},
    {-1, {"a  ", 3}, 2, jn_cut_17},
    {-1, {"a\011\011", 3}, 2, jn_cut_18},
    {-1, {"x y  z   w", 10}, 4, jn_cut_19},
    {-1, {"  \011  x", 6}, 1, jn_cut_20},
    {-1, {"abc", 3}, 1, jn_cut_21},
    {-1, {"ab cd  ef \011 gh\011", 15}, 5, jn_cut_22},
    {-1, {"\011\011", 2}, 0, 0},
    {-1, {"\011\011\011\011b   xy", 10}, 2, jn_cut_24},
    {-1, {"\011\011  ", 4}, 0, 0},
    {-1, {"\011\011  a aa", 8}, 2, jn_cut_26},
    {-1, {"\011\011aa  xya bb", 12}, 3, jn_cut_27},
    {-1, {"\011\011aaa", 5}, 1, jn_cut_28},
    {-1, {"\011\011b    a", 8}, 2, jn_cut_29},
    {-1, {"\011 ", 2}, 0, 0},
    {-1, {"\011 \011", 3}, 0, 0},
    {-1, {"\011 \011\011ba", 6}, 1, jn_cut_32},
    {-1, {"\011 \011 ba a  \011\011", 12}, 3, jn_cut_33},
    {-1, {"\011 \011 xya     xy\011", 15}, 3, jn_cut_34},
    {-1, {"\011 \011xyb \011 ", 9}, 2, jn_cut_35},
    {-1, {"\011  ", 3}, 0, 0},
    {-1, {"\011  \011ab  aabb", 12}, 2, jn_cut_37},
    {-1, {"\011     xy \011 bxy", 14}, 2, jn_cut_38},
    {-1, {"\011   a\011 xy ", 10}, 3, jn_cut_39},
    {-1, {"\011   axy    a ", 13}, 3, jn_cut_40},
    {-1, {"\011   b", 5}, 1, jn_cut_41},
    {-1, {"\011   b  \011xya  \011", 14}, 3, jn_cut_42},
    {-1, {"\011  a", 4}, 1, jn_cut_43},
    {-1, {"\011  a  ", 6}, 2, jn_cut_44},
    {-1, {"\011  aba xyb\011ab\011  ", 16}, 4, jn_cut_45},
    {-1, {"\011 a\011", 4}, 2, jn_cut_46},
    {-1, {"\011 a  xy\011  ", 10}, 3, jn_cut_47},
    {-1, {"\011 aaxya\011xyxyb", 13}, 2, jn_cut_48},
    {-1, {"\011 axya", 6}, 1, jn_cut_49},
    {-1, {"\011 xy\011    ", 9}, 2, jn_cut_50},
    {-1, {"\011a\011 aa", 6}, 2, jn_cut_51},
    {-1, {"\011a ", 3}, 2, jn_cut_52},
    {-1, {"\011a \011bxy  baa", 12}, 3, jn_cut_53},
    {-1, {"\011a   \011xy  ", 10}, 3, jn_cut_54},
    {-1, {"\011a  a     xy", 12}, 3, jn_cut_55},
    {-1, {"\011aa  ", 5}, 2, jn_cut_56},
    {-1, {"\011aa  a", 6}, 2, jn_cut_57},
    {-1, {"\011aa  aa\011 ", 9}, 3, jn_cut_58},
    {-1, {"\011aaa a\011   aa   \011", 16}, 4, jn_cut_59},
    {-1, {"\011aaaa", 5}, 1, jn_cut_60},
    {-1, {"\011aaaaxy  ", 9}, 2, jn_cut_61},
    {-1, {"\011aba ba\011   ", 11}, 3, jn_cut_62},
    {-1, {"\011b", 2}, 1, jn_cut_63},
    {-1, {"\011b\011  b\011   \011 \011bba", 16}, 3, jn_cut_64},
    {-1, {"\011b\011  b ", 7}, 3, jn_cut_65},
    {-1, {"\011b   ", 5}, 2, jn_cut_66},
    {-1, {"\011ba  ", 5}, 2, jn_cut_67},
    {-1, {"\011ba  a a   a xy  ", 17}, 6, jn_cut_68},
    {-1, {"\011baa xyba aa\011 ", 14}, 4, jn_cut_69},
    {-1, {"\011bb    a    a  a  ", 18}, 5, jn_cut_70},
    {-1, {"\011bxy xyxya  b", 13}, 3, jn_cut_71},
    {-1, {"\011xy  \011a a a   ", 14}, 5, jn_cut_72},
    {-1, {"\011xy   b", 7}, 2, jn_cut_73},
    {-1, {"\011xyaa  ", 7}, 2, jn_cut_74},
    {-1, {"\011xyba   a\011", 10}, 3, jn_cut_75},
    {-1, {" \011\011 ", 4}, 0, 0},
    {-1, {" \011\011a  xy    ", 12}, 3, jn_cut_77},
    {-1, {" \011\011a a", 6}, 2, jn_cut_78},
    {-1, {" \011  ", 4}, 0, 0},
    {-1, {" \011  \011a xyxy", 11}, 2, jn_cut_80},
    {-1, {" \011   a ", 7}, 2, jn_cut_81},
    {-1, {" \011 aa aa\011", 9}, 3, jn_cut_82},
    {-1, {" \011 b\011", 5}, 2, jn_cut_83},
    {-1, {" \011 xy a \011", 9}, 3, jn_cut_84},
    {-1, {" \011a\011xya  a ", 11}, 4, jn_cut_85},
    {-1, {" \011a     abbb  \011", 15}, 3, jn_cut_86},
    {-1, {" \011aa  ", 6}, 2, jn_cut_87},
    {-1, {" \011bb    \011 \011  ", 13}, 2, jn_cut_88},
    {-1, {" \011bxy   xy       ", 17}, 3, jn_cut_89},
    {-1, {" \011xy\011bbxy  \011b  ", 15}, 4, jn_cut_90},
    {-1, {" \011xyaxyb", 8}, 1, jn_cut_91},
    {-1, {" \011xyb\011xybb aaa ", 15}, 4, jn_cut_92},
    {-1, {" \011xyxy b  ", 10}, 3, jn_cut_93},
    {-1, {"  \011", 3}, 0, 0},
    {-1, {"  \011\011ab  b", 9}, 2, jn_cut_95},
    {-1, {"  \011\011xyaaa   baa", 15}, 2, jn_cut_96},
    {-1, {"  \011      xya  a", 15}, 2, jn_cut_97},
    {-1, {"  \011    ab", 9}, 1, jn_cut_98},
    {-1, {"  \011  a ", 7}, 2, jn_cut_99},
    {-1, {"  \011 a ", 6}, 2, jn_cut_100},
    {-1, {"  \011 a \011b      ", 14}, 3, jn_cut_101},
    {-1, {"  \011 aaaaa", 9}, 1, jn_cut_102},
    {-1, {"  \011 b  ", 7}, 2, jn_cut_103},
    {-1, {"  \011 xy \011\011      ", 15}, 2, jn_cut_104},
    {-1, {"  \011a   ", 7}, 2, jn_cut_105},
    {-1, {"  \011aa   ", 8}, 2, jn_cut_106},
    {-1, {"  \011ba\011\011bxy    abxy", 18}, 3, jn_cut_107},
    {-1, {"  \011xy", 5}, 1, jn_cut_108},
    {-1, {"   ", 3}, 0, 0},
    {-1, {"   \011", 4}, 0, 0},
    {-1, {"   \011  axy\011xyaaa\011b", 17}, 3, jn_cut_111},
    {-1, {"    ", 4}, 0, 0},
    {-1, {"    \011b   \011a\011", 12}, 3, jn_cut_113},
    {-1, {"    \011b aaa   ", 13}, 3, jn_cut_114},
    {-1, {"        a  ", 11}, 2, jn_cut_115},
    {-1, {"      aaxy   aab", 16}, 2, jn_cut_116},
    {-1, {"      bb\011b    bxy", 17}, 3, jn_cut_117},
    {-1, {"     a", 6}, 1, jn_cut_118},
    {-1, {"     a   \011a\011aa \011  ", 18}, 4, jn_cut_119},
    {-1, {"     a  xy", 10}, 2, jn_cut_120},
    {-1, {"     a axy", 10}, 2, jn_cut_121},
    {-1, {"     a bxy ", 11}, 3, jn_cut_122},
    {-1, {"     aa\011 ", 9}, 2, jn_cut_123},
    {-1, {"     axy", 8}, 1, jn_cut_124},
    {-1, {"     b\011  b a  ", 14}, 4, jn_cut_125},
    {-1, {"     xy\011 \011aaxya", 15}, 2, jn_cut_126},
    {-1, {"    a\011xy a  b  ", 15}, 5, jn_cut_127},
    {-1, {"    aaa  ", 9}, 2, jn_cut_128},
    {-1, {"    aba\011b  a  a  ", 17}, 5, jn_cut_129},
    {-1, {"    abaab a ", 12}, 3, jn_cut_130},
    {-1, {"    axyxy", 9}, 1, jn_cut_131},
    {-1, {"    b bxy", 9}, 2, jn_cut_132},
    {-1, {"    xy\011a\011 bb", 12}, 3, jn_cut_133},
    {-1, {"    xyaa\011   aaxya ", 18}, 3, jn_cut_134},
    {-1, {"    xybaabxya  a  ba", 20}, 3, jn_cut_135},
    {-1, {"   a    xy  ", 12}, 3, jn_cut_136},
    {-1, {"   a  axya\011", 11}, 3, jn_cut_137},
    {-1, {"   aa\011  aaxy", 12}, 2, jn_cut_138},
    {-1, {"   aaaabxy a\011b", 14}, 3, jn_cut_139},
    {-1, {"   axyab  aa", 12}, 2, jn_cut_140},
    {-1, {"   axyaxy", 9}, 1, jn_cut_141},
    {-1, {"   b \011   aba", 12}, 2, jn_cut_142},
    {-1, {"   b  a", 7}, 2, jn_cut_143},
    {-1, {"   xy", 5}, 1, jn_cut_144},
    {-1, {"   xy   bba ", 12}, 3, jn_cut_145},
    {-1, {"   xy xyab xy", 13}, 3, jn_cut_146},
    {-1, {"   xyaaa\011     \011", 15}, 2, jn_cut_147},
    {-1, {"  a", 3}, 1, jn_cut_148},
    {-1, {"  a\011\011bbaaa", 10}, 2, jn_cut_149},
    {-1, {"  a\011   ab  b\011", 13}, 4, jn_cut_150},
    {-1, {"  a   \011xyxy xyb\011a ", 18}, 5, jn_cut_151},
    {-1, {"  a    xy ", 10}, 3, jn_cut_152},
    {-1, {"  a  a    \011ba   \011\011 ", 19}, 4, jn_cut_153},
    {-1, {"  a xy \011xyxya  ", 15}, 4, jn_cut_154},
    {-1, {"  aa  ", 6}, 2, jn_cut_155},
    {-1, {"  aa   aab", 10}, 2, jn_cut_156},
    {-1, {"  aaaaaba    ", 13}, 2, jn_cut_157},
    {-1, {"  aaxyaa aaa a\011", 15}, 4, jn_cut_158},
    {-1, {"  ab    axy\011 abaa", 17}, 3, jn_cut_159},
    {-1, {"  ab a    xyb  ab", 17}, 4, jn_cut_160},
    {-1, {"  aba ", 6}, 2, jn_cut_161},
    {-1, {"  aba \011xy \011xyxy ", 16}, 4, jn_cut_162},
    {-1, {"  axy    xya\011", 13}, 3, jn_cut_163},
    {-1, {"  axyxy    bxy", 14}, 2, jn_cut_164},
    {-1, {"  b\011 \011  \011\011 \011    ", 16}, 2, jn_cut_165},
    {-1, {"  b\011    \011  abaxy\011", 17}, 3, jn_cut_166},
    {-1, {"  b   b   a", 11}, 3, jn_cut_167},
    {-1, {"  b  xy", 7}, 2, jn_cut_168},
    {-1, {"  ba    ", 8}, 2, jn_cut_169},
    {-1, {"  baba b a  a", 13}, 4, jn_cut_170},
    {-1, {"  bb   a  aaxy", 14}, 3, jn_cut_171},
    {-1, {"  xy \011\011\011xyba ", 13}, 3, jn_cut_172},
    {-1, {"  xy \011b  \011", 10}, 3, jn_cut_173},
    {-1, {"  xy   ", 7}, 2, jn_cut_174},
    {-1, {"  xy    \011 \011 \011", 13}, 2, jn_cut_175},
    {-1, {"  xy ab\011ab", 10}, 3, jn_cut_176},
    {-1, {"  xya   baxy b", 14}, 3, jn_cut_177},
    {-1, {" a\011  axyxy  xy   ", 17}, 4, jn_cut_178},
    {-1, {" a\011b  ", 6}, 3, jn_cut_179},
    {-1, {" a  ", 4}, 2, jn_cut_180},
    {-1, {" a    ba\011aaxy  ", 15}, 4, jn_cut_181},
    {-1, {" a  xy", 6}, 2, jn_cut_182},
    {-1, {" a a   xy    a", 14}, 4, jn_cut_183},
    {-1, {" a a a    \011xy", 13}, 4, jn_cut_184},
    {-1, {" aa \011 b", 7}, 2, jn_cut_185},
    {-1, {" aa a  ", 7}, 3, jn_cut_186},
    {-1, {" aaa    ab  baa", 15}, 3, jn_cut_187},
    {-1, {" aaa a xyb", 10}, 3, jn_cut_188},
    {-1, {" aaaxy \011xy   a  b", 17}, 4, jn_cut_189},
    {-1, {" aabb", 5}, 1, jn_cut_190},
    {-1, {" ab  a aba  \011  a", 16}, 4, jn_cut_191},
    {-1, {" abbaxy   aa ab", 15}, 3, jn_cut_192},
    {-1, {" axya  aa  xy\011a", 15}, 4, jn_cut_193},
    {-1, {" axyaaab\011 ", 10}, 2, jn_cut_194},
    {-1, {" axyab", 6}, 1, jn_cut_195},
    {-1, {" axybxy xy bxy", 14}, 3, jn_cut_196},
    {-1, {" axyxya", 7}, 1, jn_cut_197},
    {-1, {" b\011 a  xya", 10}, 3, jn_cut_198},
    {-1, {" b ", 3}, 2, jn_cut_199},
    {-1, {" b  ", 4}, 2, jn_cut_200},
    {-1, {" b    ", 6}, 2, jn_cut_201},
    {-1, {" b   b\011", 7}, 3, jn_cut_202},
    {-1, {" b xyaxy  aba", 13}, 3, jn_cut_203},
    {-1, {" bb  a\011   a ", 12}, 4, jn_cut_204},
    {-1, {" bxybaa abb", 11}, 2, jn_cut_205},
    {-1, {" xy\011\011 a  xyaaa   ", 17}, 4, jn_cut_206},
    {-1, {" xy\011a\011", 6}, 3, jn_cut_207},
    {-1, {" xy \011a b  \011 \011b", 14}, 4, jn_cut_208},
    {-1, {" xy aaa ab", 10}, 3, jn_cut_209},
    {-1, {" xya   a   b\011a\011a  ", 18}, 6, jn_cut_210},
    {-1, {" xyaa\011b    b \011 ", 15}, 4, jn_cut_211},
    {-1, {" xyb xyxy", 9}, 2, jn_cut_212},
    {-1, {" xybxyxy \011  axy", 15}, 2, jn_cut_213},
    {-1, {" xyxy\011xy ", 9}, 3, jn_cut_214},
    {-1, {" xyxyb ", 7}, 2, jn_cut_215},
    {-1, {"a\011", 2}, 2, jn_cut_216},
    {-1, {"a\011\011  aa \011  aa ", 14}, 4, jn_cut_217},
    {-1, {"a\011\011 b axy  ba", 13}, 4, jn_cut_218},
    {-1, {"a\011\011 xyaa     \011xy ", 17}, 4, jn_cut_219},
    {-1, {"a\011  \011aa a", 9}, 3, jn_cut_220},
    {-1, {"a\011   a ", 7}, 3, jn_cut_221},
    {-1, {"a\011   b", 6}, 2, jn_cut_222},
    {-1, {"a\011  bxya xy a\011", 14}, 5, jn_cut_223},
    {-1, {"a\011 a ", 5}, 3, jn_cut_224},
    {-1, {"a\011a  xya", 8}, 3, jn_cut_225},
    {-1, {"a\011aaaa ba  ", 11}, 4, jn_cut_226},
    {-1, {"a\011aba", 5}, 2, jn_cut_227},
    {-1, {"a\011axy", 5}, 2, jn_cut_228},
    {-1, {"a\011bxyxy\011", 8}, 3, jn_cut_229},
    {-1, {"a \011 ", 4}, 2, jn_cut_230},
    {-1, {"a \011 xy", 6}, 2, jn_cut_231},
    {-1, {"a   ", 4}, 2, jn_cut_232},
    {-1, {"a    \011b ", 8}, 3, jn_cut_233},
    {-1, {"a     \011         b", 17}, 2, jn_cut_234},
    {-1, {"a       xyb ", 12}, 3, jn_cut_235},
    {-1, {"a     a   axy      ", 19}, 4, jn_cut_236},
    {-1, {"a    a  b", 9}, 3, jn_cut_237},
    {-1, {"a   b\011   ", 9}, 3, jn_cut_238},
    {-1, {"a   xya", 7}, 2, jn_cut_239},
    {-1, {"a   xyxy bb ", 12}, 4, jn_cut_240},
    {-1, {"a  a\011", 5}, 3, jn_cut_241},
    {-1, {"a  a ", 5}, 3, jn_cut_242},
    {-1, {"a  a    ", 8}, 3, jn_cut_243},
    {-1, {"a  aa xyaa ba  ", 15}, 5, jn_cut_244},
    {-1, {"a  bb  \011", 8}, 3, jn_cut_245},
    {-1, {"a  xy\011  xy ", 11}, 4, jn_cut_246},
    {-1, {"a a", 3}, 2, jn_cut_247},
    {-1, {"a a  a", 6}, 3, jn_cut_248},
    {-1, {"a a  a ", 7}, 4, jn_cut_249},
    {-1, {"a axy a  ", 9}, 4, jn_cut_250},
    {-1, {"a b\011a  b aab", 12}, 5, jn_cut_251},
    {-1, {"a b  \011", 6}, 3, jn_cut_252},
    {-1, {"a b  a", 6}, 3, jn_cut_253},
    {-1, {"a bb", 4}, 2, jn_cut_254},
    {-1, {"aa", 2}, 1, jn_cut_255},
    {-1, {"aa\011", 3}, 2, jn_cut_256},
    {-1, {"aa\011     \011a", 10}, 2, jn_cut_257},
    {-1, {"aa\011a", 4}, 2, jn_cut_258},
    {-1, {"aa\011b\011  ", 7}, 3, jn_cut_259},
    {-1, {"aa \011abaaxybbb", 13}, 2, jn_cut_260},
    {-1, {"aa aa xyxy\011a     ", 17}, 5, jn_cut_261},
    {-1, {"aa xyaxy a", 10}, 3, jn_cut_262},
    {-1, {"aaa", 3}, 1, jn_cut_263},
    {-1, {"aaaa  a  ab", 11}, 3, jn_cut_264},
    {-1, {"aaaaa  baa\011 a", 13}, 3, jn_cut_265},
    {-1, {"aaaaxyxya\011xyaxy", 15}, 2, jn_cut_266},
    {-1, {"aab\011", 4}, 2, jn_cut_267},
    {-1, {"aab \011", 5}, 2, jn_cut_268},
    {-1, {"aabb\011b", 6}, 2, jn_cut_269},
    {-1, {"aaxy xy\011aaxyb bxy", 17}, 4, jn_cut_270},
    {-1, {"ab\011axy", 6}, 2, jn_cut_271},
    {-1, {"ab  \011axy\011 axyaxya\011", 18}, 4, jn_cut_272},
    {-1, {"ab   a\011b xy     a", 17}, 5, jn_cut_273},
    {-1, {"ab  aa  b a  a", 14}, 5, jn_cut_274},
    {-1, {"ab  bb\011", 7}, 3, jn_cut_275},
    {-1, {"aba", 3}, 1, jn_cut_276},
    {-1, {"aba bxyab\011", 10}, 3, jn_cut_277},
    {-1, {"abaa baa ", 9}, 3, jn_cut_278},
    {-1, {"abaxya\011aaa\011", 11}, 3, jn_cut_279},
    {-1, {"abb \011 a", 7}, 2, jn_cut_280},
    {-1, {"abxy", 4}, 1, jn_cut_281},
    {-1, {"abxy\011bab\011\011\011b", 12}, 3, jn_cut_282},
    {-1, {"abxy   aa", 9}, 2, jn_cut_283},
    {-1, {"axy\011    a", 9}, 2, jn_cut_284},
    {-1, {"axy        aaxyaaa ", 19}, 3, jn_cut_285},
    {-1, {"axy     aa a", 12}, 3, jn_cut_286},
    {-1, {"axy  a", 6}, 2, jn_cut_287},
    {-1, {"axy  b\011  a\011a \011  b", 17}, 5, jn_cut_288},
    {-1, {"axy  xy  \011 \011b", 13}, 3, jn_cut_289},
    {-1, {"axya\011xyxya ", 11}, 3, jn_cut_290},
    {-1, {"axyabaxya  \011   \011\011 ", 18}, 2, jn_cut_291},
    {-1, {"axyaxyaba   a", 13}, 2, jn_cut_292},
    {-1, {"axyb   a\011a", 10}, 3, jn_cut_293},
    {-1, {"axyb a", 6}, 2, jn_cut_294},
    {-1, {"axyba   a  \011aaxy  ", 18}, 4, jn_cut_295},
    {-1, {"axyxy", 5}, 1, jn_cut_296},
    {-1, {"axyxy   baa    \011 ", 17}, 3, jn_cut_297},
    {-1, {"axyxy  bab  b", 13}, 3, jn_cut_298},
    {-1, {"axyxya \011 a", 10}, 2, jn_cut_299},
    {-1, {"b", 1}, 1, jn_cut_300},
    {-1, {"b\011\011a abbxy a\011  ", 15}, 5, jn_cut_301},
    {-1, {"b\011     ", 7}, 2, jn_cut_302},
    {-1, {"b\011   axyaba", 11}, 2, jn_cut_303},
    {-1, {"b\011 a\011xy ", 8}, 4, jn_cut_304},
    {-1, {"b\011baa\011  ", 8}, 3, jn_cut_305},
    {-1, {"b\011baab", 6}, 2, jn_cut_306},
    {-1, {"b\011baxya \011a ", 11}, 4, jn_cut_307},
    {-1, {"b ", 2}, 2, jn_cut_308},
    {-1, {"b \011\011a", 5}, 2, jn_cut_309},
    {-1, {"b       xyb\011b", 13}, 3, jn_cut_310},
    {-1, {"b   a\011\011b", 8}, 3, jn_cut_311},
    {-1, {"b   b  ", 7}, 3, jn_cut_312},
    {-1, {"b   b  \011\011\011  ", 12}, 3, jn_cut_313},
    {-1, {"b   b b\011  xyxya  a", 18}, 5, jn_cut_314},
    {-1, {"b   ba     axya xy", 18}, 4, jn_cut_315},
    {-1, {"b   xy   ", 9}, 3, jn_cut_316},
    {-1, {"b   xy   bxy\011", 13}, 4, jn_cut_317},
    {-1, {"b  a a\011b\011  b  ", 14}, 6, jn_cut_318},
    {-1, {"b  babb  ", 9}, 3, jn_cut_319},
    {-1, {"b  bb a \011 ", 10}, 4, jn_cut_320},
    {-1, {"b a\011\011a  \011  \011xy  ", 16}, 5, jn_cut_321},
    {-1, {"b a  b    bbba ", 15}, 5, jn_cut_322},
    {-1, {"b ab", 4}, 2, jn_cut_323},
    {-1, {"b ab xy\011    \011  ", 15}, 4, jn_cut_324},
    {-1, {"b aba \011 xya", 11}, 3, jn_cut_325},
    {-1, {"b axya   a\011b   a", 16}, 5, jn_cut_326},
    {-1, {"b b\011a      a  ", 14}, 5, jn_cut_327},
    {-1, {"b bb a aaaba", 12}, 4, jn_cut_328},
    {-1, {"b xy", 4}, 2, jn_cut_329},
    {-1, {"b xy     \011ba  ", 14}, 4, jn_cut_330},
    {-1, {"b xyaa\011axyaa\011 ", 14}, 4, jn_cut_331},
    {-1, {"b xyxy\011 bxy xy", 14}, 4, jn_cut_332},
    {-1, {"ba\011 a\011a   b  a a", 16}, 6, jn_cut_333},
    {-1, {"ba ", 3}, 2, jn_cut_334},
    {-1, {"ba  \011\011", 6}, 2, jn_cut_335},
    {-1, {"ba  xyb", 7}, 2, jn_cut_336},
    {-1, {"ba b  a  \011  \011 ", 14}, 4, jn_cut_337},
    {-1, {"baa  bxy  ", 10}, 3, jn_cut_338},
    {-1, {"bab", 3}, 1, jn_cut_339},
    {-1, {"baxy", 4}, 1, jn_cut_340},
    {-1, {"baxy ba\011\011  ", 11}, 3, jn_cut_341},
    {-1, {"bb  ", 4}, 2, jn_cut_342},
    {-1, {"bb      a", 9}, 2, jn_cut_343},
    {-1, {"bba\011 ", 5}, 2, jn_cut_344},
    {-1, {"bxy", 3}, 1, jn_cut_345},
    {-1, {"bxy\011 aaa\011aa a\011", 14}, 5, jn_cut_346},
    {-1, {"bxy\011xy  \011aa", 11}, 3, jn_cut_347},
    {-1, {"bxy  b a", 8}, 3, jn_cut_348},
    {-1, {"bxya    aaa", 11}, 2, jn_cut_349},
    {-1, {"bxyb\011a  xy ", 11}, 4, jn_cut_350},
    {-1, {"bxyxy", 5}, 1, jn_cut_351},
    {-1, {"bxyxybaab", 9}, 1, jn_cut_352},
    {-1, {"xy", 2}, 1, jn_cut_353},
    {-1, {"xy\011 \011", 5}, 2, jn_cut_354},
    {-1, {"xy\011  ", 5}, 2, jn_cut_355},
    {-1, {"xy\011a\011   ", 8}, 3, jn_cut_356},
    {-1, {"xy\011a  \011", 7}, 3, jn_cut_357},
    {-1, {"xy\011axy\011a  aaa", 13}, 4, jn_cut_358},
    {-1, {"xy\011xy", 5}, 2, jn_cut_359},
    {-1, {"xy \011xyxy     \011  aa b", 20}, 4, jn_cut_360},
    {-1, {"xy  \011aabaa", 10}, 2, jn_cut_361},
    {-1, {"xy       a\011", 11}, 3, jn_cut_362},
    {-1, {"xy     xyaab xyaa", 17}, 3, jn_cut_363},
    {-1, {"xy   axy\011  ", 11}, 3, jn_cut_364},
    {-1, {"xy   b\011a", 8}, 3, jn_cut_365},
    {-1, {"xy   xyxy\011 ", 11}, 3, jn_cut_366},
    {-1, {"xy  aabxyxyxy  b", 16}, 3, jn_cut_367},
    {-1, {"xy  babb  ", 10}, 3, jn_cut_368},
    {-1, {"xy  xy\011 b", 9}, 3, jn_cut_369},
    {-1, {"xy  xy    bb", 12}, 3, jn_cut_370},
    {-1, {"xy  xyaaxya a   ", 16}, 4, jn_cut_371},
    {-1, {"xy a   aaxyxya", 14}, 3, jn_cut_372},
    {-1, {"xy a aaaab", 10}, 3, jn_cut_373},
    {-1, {"xy a abaxy", 10}, 3, jn_cut_374},
    {-1, {"xy aa xyb\011 ", 11}, 4, jn_cut_375},
    {-1, {"xy aaab", 7}, 2, jn_cut_376},
    {-1, {"xy abb  b", 9}, 3, jn_cut_377},
    {-1, {"xy ba", 5}, 2, jn_cut_378},
    {-1, {"xy ba \011", 7}, 3, jn_cut_379},
    {-1, {"xy bxy\011\011b  ", 11}, 4, jn_cut_380},
    {-1, {"xy xy\011xy\011\011\011", 11}, 4, jn_cut_381},
    {-1, {"xy xy a  xya\011a", 14}, 5, jn_cut_382},
    {-1, {"xya ", 4}, 2, jn_cut_383},
    {-1, {"xya \011a xybxy", 12}, 3, jn_cut_384},
    {-1, {"xya   \011\011a abxy", 14}, 3, jn_cut_385},
    {-1, {"xya   a\011aaa", 11}, 3, jn_cut_386},
    {-1, {"xya  xy", 7}, 2, jn_cut_387},
    {-1, {"xya xyaxy bb", 12}, 3, jn_cut_388},
    {-1, {"xyaa \011axyb   ", 13}, 3, jn_cut_389},
    {-1, {"xyaaaxy\011aa\011aa", 13}, 3, jn_cut_390},
    {-1, {"xyab", 4}, 1, jn_cut_391},
    {-1, {"xyab\011a", 6}, 2, jn_cut_392},
    {-1, {"xyaxy\011 a  b\011 ", 13}, 4, jn_cut_393},
    {-1, {"xyaxy  aaxy  bb", 15}, 3, jn_cut_394},
    {-1, {"xyaxy xy\011   a     xy", 20}, 4, jn_cut_395},
    {-1, {"xyaxya\011\011", 8}, 2, jn_cut_396},
    {-1, {"xyb\011b   xya b\011   ", 17}, 5, jn_cut_397},
    {-1, {"xyb  a ab  ab  ", 15}, 5, jn_cut_398},
    {-1, {"xyb a  a  a xy\011 b", 17}, 6, jn_cut_399},
    {-1, {"xyba", 4}, 1, jn_cut_400},
    {-1, {"xyba ab", 7}, 2, jn_cut_401},
    {-1, {"xybb\011axya", 9}, 2, jn_cut_402},
    {-1, {"xybba\011  ", 8}, 2, jn_cut_403},
    {-1, {"xybxyaxyabb", 11}, 1, jn_cut_404},
    {-1, {"xybxyxyba\011  xy\011 a ", 18}, 4, jn_cut_405},
    {-1, {"xyxy", 4}, 1, jn_cut_406},
    {-1, {"xyxy\011xyaxyaxyxy xy\011", 19}, 4, jn_cut_407},
    {-1, {"xyxy ", 5}, 2, jn_cut_408},
    {-1, {"xyxy \011  b", 9}, 2, jn_cut_409},
    {-1, {"xyxy  abxy ", 11}, 3, jn_cut_410},
    {-1, {"xyxyb\011b\011 \011xy\011 a", 15}, 4, jn_cut_411},
    {-1, {"a\012b", 3}, 2, jn_cut_412},
    {-1, {"\012", 1}, 0, 0},
    {-1, {"a\012", 2}, 2, jn_cut_414},
    {-1, {"\012a", 2}, 1, jn_cut_415},
    {-1, {"a \012 b", 5}, 2, jn_cut_416},
    {-1, {" \012 ", 3}, 0, 0},
    {-1, {"x\012\012y", 4}, 2, jn_cut_418},
    {-1, {"\012\012", 2}, 0, 0},
    {-1, {"a b\012c d", 7}, 4, jn_cut_420},
    {-1, {"", 0}, 0, 0},
    {-1, {"\011", 1}, 0, 0},
    {-1, {"\011\011\012", 3}, 0, 0},
    {-1, {"\011\011b\012xy xybb", 11}, 3, jn_cut_424},
    {-1, {"\011\011xyxy", 6}, 1, jn_cut_425},
    {-1, {"\011\012\012\011bxy", 7}, 1, jn_cut_426},
    {-1, {"\011\012 \011b ", 6}, 2, jn_cut_427},
    {-1, {"\011\012b\011\011xyxy\012\011b", 12}, 3, jn_cut_428},
    {-1, {"\011\012xyaxy\011\011 xya", 13}, 2, jn_cut_429},
    {-1, {"\011\012xybxyxy \011b", 12}, 2, jn_cut_430},
    {-1, {"\011 ", 2}, 0, 0},
    {-1, {"\011 a \011\012aa\011", 9}, 3, jn_cut_432},
    {-1, {"\011 ab\011a\011axy", 10}, 3, jn_cut_433},
    {-1, {"\011 axy", 5}, 1, jn_cut_434},
    {-1, {"\011 b\012axyxy", 9}, 2, jn_cut_435},
    {-1, {"\011 bxyxyxy\011\012 xy", 14}, 2, jn_cut_436},
    {-1, {"\011aa\012b", 5}, 2, jn_cut_437},
    {-1, {"\011axy\011a", 6}, 2, jn_cut_438},
    {-1, {"\011ba\012\012", 5}, 2, jn_cut_439},
    {-1, {"\011xy", 3}, 1, jn_cut_440},
    {-1, {"\011xy\011\012ba\011\012\011a", 11}, 3, jn_cut_441},
    {-1, {"\011xy\012", 4}, 2, jn_cut_442},
    {-1, {"\011xy  a", 6}, 2, jn_cut_443},
    {-1, {"\011xy bb xya", 10}, 3, jn_cut_444},
    {-1, {"\011xy xy  ", 8}, 3, jn_cut_445},
    {-1, {"\011xya\011ba \011b", 10}, 3, jn_cut_446},
    {-1, {"\011xyab", 5}, 1, jn_cut_447},
    {-1, {"\011xyba\011xy ", 9}, 3, jn_cut_448},
    {-1, {"\012\011", 2}, 0, 0},
    {-1, {"\012\011 \011 ", 5}, 0, 0},
    {-1, {"\012\011b ", 4}, 2, jn_cut_451},
    {-1, {"\012\011xyaxyxy", 9}, 1, jn_cut_452},
    {-1, {"\012\011xyxy", 6}, 1, jn_cut_453},
    {-1, {"\012\012\012", 3}, 0, 0},
    {-1, {"\012\012 \011\011xya\012xyb", 12}, 2, jn_cut_455},
    {-1, {"\012\012 baaa", 7}, 1, jn_cut_456},
    {-1, {"\012\012aa\012\011a\011b", 9}, 3, jn_cut_457},
    {-1, {"\012 \012 xyb\012xy  ", 12}, 3, jn_cut_458},
    {-1, {"\012 \012bb a xy", 10}, 3, jn_cut_459},
    {-1, {"\012 a\012\011aaa\012", 9}, 3, jn_cut_460},
    {-1, {"\012 xy a", 6}, 2, jn_cut_461},
    {-1, {"\012a  \012\012\011\011xy", 10}, 2, jn_cut_462},
    {-1, {"\012ab\012b\012", 6}, 3, jn_cut_463},
    {-1, {"\012b", 2}, 1, jn_cut_464},
    {-1, {"\012ba\011b", 5}, 2, jn_cut_465},
    {-1, {"\012bb\011\012aab", 8}, 2, jn_cut_466},
    {-1, {"\012xy xy xya", 10}, 3, jn_cut_467},
    {-1, {"\012xyxya\011xy\011", 10}, 3, jn_cut_468},
    {-1, {" ", 1}, 0, 0},
    {-1, {" \011 ", 3}, 0, 0},
    {-1, {" \011 \011", 4}, 0, 0},
    {-1, {" \011    xyxyxy", 12}, 1, jn_cut_472},
    {-1, {" \012a", 3}, 1, jn_cut_473},
    {-1, {" \012xyxyxy\012bxya\012", 14}, 3, jn_cut_474},
    {-1, {"  ", 2}, 0, 0},
    {-1, {"  \012  ba\012a", 9}, 2, jn_cut_476},
    {-1, {"    b", 5}, 1, jn_cut_477},
    {-1, {"  b\012", 4}, 2, jn_cut_478},
    {-1, {" a\011", 3}, 2, jn_cut_479},
    {-1, {" a\012", 3}, 2, jn_cut_480},
    {-1, {" aaxya\012", 7}, 2, jn_cut_481},
    {-1, {" aaxyb", 6}, 1, jn_cut_482},
    {-1, {" ab\012\011\012\011", 7}, 2, jn_cut_483},
    {-1, {" axy", 4}, 1, jn_cut_484},
    {-1, {" b a\011\011", 6}, 3, jn_cut_485},
    {-1, {" ba\011b", 5}, 2, jn_cut_486},
    {-1, {" bb\012ab xy", 9}, 3, jn_cut_487},
    {-1, {" bxy\011 a", 7}, 2, jn_cut_488},
    {-1, {" bxyb\012", 6}, 2, jn_cut_489},
    {-1, {" xy ", 4}, 2, jn_cut_490},
    {-1, {" xya\012\012b", 7}, 2, jn_cut_491},
    {-1, {" xya \011aa\012", 9}, 3, jn_cut_492},
    {-1, {"a", 1}, 1, jn_cut_493},
    {-1, {"a\011\011bb\011", 6}, 3, jn_cut_494},
    {-1, {"a\011\012", 3}, 2, jn_cut_495},
    {-1, {"a\011 ", 3}, 2, jn_cut_496},
    {-1, {"a\011b\011  b bb", 10}, 4, jn_cut_497},
    {-1, {"a\011bb\012axyxy", 10}, 3, jn_cut_498},
    {-1, {"a\012\011\011 \012bxy\012xy", 12}, 3, jn_cut_499},
    {-1, {"a\012\012\012\012\012\012axya", 11}, 2, jn_cut_500},
    {-1, {"a\012\012 xyaaxyb", 11}, 2, jn_cut_501},
    {-1, {"a\012a\012xyb", 7}, 3, jn_cut_502},
    {-1, {"a ", 2}, 2, jn_cut_503},
    {-1, {"a \011 xy\011", 7}, 3, jn_cut_504},
    {-1, {"a \011ab", 5}, 2, jn_cut_505},
    {-1, {"a \011ba b", 7}, 3, jn_cut_506},
    {-1, {"a axy\012aa b", 10}, 4, jn_cut_507},
    {-1, {"a bxy\011", 6}, 3, jn_cut_508},
    {-1, {"aa", 2}, 1, jn_cut_509},
    {-1, {"aa\011", 3}, 2, jn_cut_510},
    {-1, {"aa\011\012xy\011a", 8}, 3, jn_cut_511},
    {-1, {"aaa\011ab", 6}, 2, jn_cut_512},
    {-1, {"aab", 3}, 1, jn_cut_513},
    {-1, {"aaba  \011", 7}, 2, jn_cut_514},
    {-1, {"aaxyb", 5}, 1, jn_cut_515},
    {-1, {"ab \011\011", 5}, 2, jn_cut_516},
    {-1, {"abb", 3}, 1, jn_cut_517},
    {-1, {"abb \011abxybb", 11}, 2, jn_cut_518},
    {-1, {"axy\011xy\012xy \011", 11}, 4, jn_cut_519},
    {-1, {"axy\011xyxya\012xy", 12}, 3, jn_cut_520},
    {-1, {"axyxy", 5}, 1, jn_cut_521},
    {-1, {"b", 1}, 1, jn_cut_522},
    {-1, {"b\011\012xyxyaxyb", 11}, 2, jn_cut_523},
    {-1, {"b\011bbaa", 6}, 2, jn_cut_524},
    {-1, {"b\011xyxyxybxy", 11}, 2, jn_cut_525},
    {-1, {"b\012\011\011\011\012\012xyb", 10}, 2, jn_cut_526},
    {-1, {"b\012 \011\011axy", 8}, 2, jn_cut_527},
    {-1, {"b\012  a", 5}, 2, jn_cut_528},
    {-1, {"b ", 2}, 2, jn_cut_529},
    {-1, {"b \012aa\012b", 7}, 3, jn_cut_530},
    {-1, {"b  \012", 4}, 2, jn_cut_531},
    {-1, {"b ab \012\011xy\012xy", 12}, 4, jn_cut_532},
    {-1, {"ba\011", 3}, 2, jn_cut_533},
    {-1, {"ba\011axyba \011", 10}, 3, jn_cut_534},
    {-1, {"ba\012", 3}, 2, jn_cut_535},
    {-1, {"ba \011\011\012a ", 8}, 3, jn_cut_536},
    {-1, {"baa\011 ", 5}, 2, jn_cut_537},
    {-1, {"baxy\012", 5}, 2, jn_cut_538},
    {-1, {"bb\012b \012\012xya", 10}, 3, jn_cut_539},
    {-1, {"bbxya\012xy", 8}, 2, jn_cut_540},
    {-1, {"bxy\012xy xy", 9}, 3, jn_cut_541},
    {-1, {"bxy a\012 \011 ", 9}, 3, jn_cut_542},
    {-1, {"bxyxy\011\012  ", 9}, 2, jn_cut_543},
    {-1, {"bxyxyb b\012", 9}, 3, jn_cut_544},
    {-1, {"xy", 2}, 1, jn_cut_545},
    {-1, {"xy\011a", 4}, 2, jn_cut_546},
    {-1, {"xy\011xya bxybxy", 13}, 3, jn_cut_547},
    {-1, {"xy\011xyb\012bb\011", 10}, 4, jn_cut_548},
    {-1, {"xy\012\012", 4}, 2, jn_cut_549},
    {-1, {"xy\012 xy bab\011a", 12}, 4, jn_cut_550},
    {-1, {"xy\012babxy", 8}, 2, jn_cut_551},
    {-1, {"xy\012xy\011b xy\011", 11}, 5, jn_cut_552},
    {-1, {"xy ", 3}, 2, jn_cut_553},
    {-1, {"xy \011\011abxyxy xy", 14}, 3, jn_cut_554},
    {-1, {"xy  \012", 5}, 2, jn_cut_555},
    {-1, {"xy  \012a", 6}, 2, jn_cut_556},
    {-1, {"xy aa", 5}, 2, jn_cut_557},
    {-1, {"xya\012a", 5}, 2, jn_cut_558},
    {-1, {"xyaa b\011axyxy", 12}, 3, jn_cut_559},
    {-1, {"xyabxy", 6}, 1, jn_cut_560},
    {-1, {"xyaxyxy", 7}, 1, jn_cut_561},
    {-1, {"xyb\011 \012a\011", 8}, 3, jn_cut_562},
    {-1, {"xyb\011ab\012 \011 xy", 12}, 3, jn_cut_563},
    {-1, {"xyb\012", 4}, 2, jn_cut_564},
    {-1, {"xyb\012\012\011aa", 8}, 2, jn_cut_565},
    {-1, {"xybxyxyxyxyaba", 14}, 1, jn_cut_566},
    {-1, {"xyxya\011\011xy \011\011\011", 13}, 3, jn_cut_567},
    {-1, {"xyxybxyb", 8}, 1, jn_cut_568},
    {-1, {"xyxyxy\011a\012xyxya", 14}, 3, jn_cut_569},
    {44, {"", 0}, 0, 0},
    {44, {",", 1}, 2, jn_cut_571},
    {44, {",,", 2}, 3, jn_cut_572},
    {44, {"a", 1}, 1, jn_cut_573},
    {44, {"a,", 2}, 2, jn_cut_574},
    {44, {",a", 2}, 2, jn_cut_575},
    {44, {"a,b", 3}, 2, jn_cut_576},
    {44, {"a,,b", 4}, 3, jn_cut_577},
    {44, {" ", 1}, 1, jn_cut_578},
    {44, {"  ", 2}, 1, jn_cut_579},
    {44, {"  , ,,a", 7}, 4, jn_cut_580},
    {44, {"  bab,,abxy", 11}, 3, jn_cut_581},
    {44, {" ,,,,b,, ,", 10}, 8, jn_cut_582},
    {44, {" ,,,a b ,,", 10}, 6, jn_cut_583},
    {44, {" ,,,a, ,", 8}, 6, jn_cut_584},
    {44, {" ,,bb,", 6}, 4, jn_cut_585},
    {44, {" ,xyaxy,,bb ", 12}, 4, jn_cut_586},
    {44, {" a,,,,,,axy,,", 13}, 9, jn_cut_587},
    {44, {" a,,,xya", 8}, 4, jn_cut_588},
    {44, {" aab", 4}, 1, jn_cut_589},
    {44, {" b", 2}, 1, jn_cut_590},
    {44, {" ba", 3}, 1, jn_cut_591},
    {44, {" bxy,,abb,,", 11}, 5, jn_cut_592},
    {44, {", ,,", 4}, 4, jn_cut_593},
    {44, {", b", 3}, 2, jn_cut_594},
    {44, {",, ,,axy a ", 11}, 5, jn_cut_595},
    {44, {",, xyxyaxy,,xy", 14}, 5, jn_cut_596},
    {44, {",,, ,,xya,, ", 12}, 8, jn_cut_597},
    {44, {",,,,", 4}, 5, jn_cut_598},
    {44, {",,,, ", 5}, 5, jn_cut_599},
    {44, {",,,,  a,xy", 10}, 6, jn_cut_600},
    {44, {",,,,  xy,,,b,", 13}, 9, jn_cut_601},
    {44, {",,,,,,,xy", 9}, 8, jn_cut_602},
    {44, {",,,a,, ba b", 11}, 6, jn_cut_603},
    {44, {",,,b bxyaxyxya", 14}, 4, jn_cut_604},
    {44, {",,,baxyb xy,xy", 14}, 5, jn_cut_605},
    {44, {",,,xy  xyb", 10}, 4, jn_cut_606},
    {44, {",,,xy,bxy", 9}, 5, jn_cut_607},
    {44, {",,,xyaxy", 8}, 4, jn_cut_608},
    {44, {",,a", 3}, 3, jn_cut_609},
    {44, {",,a,", 4}, 4, jn_cut_610},
    {44, {",,aaabxy,xy, ", 13}, 5, jn_cut_611},
    {44, {",,abxy ,", 8}, 4, jn_cut_612},
    {44, {",,b", 3}, 3, jn_cut_613},
    {44, {",,b  xy,b,  ", 12}, 5, jn_cut_614},
    {44, {",,b xy", 6}, 3, jn_cut_615},
    {44, {",,b xy,,xy", 10}, 5, jn_cut_616},
    {44, {",,bxya,b", 8}, 4, jn_cut_617},
    {44, {",,bxyxy ,,b", 11}, 5, jn_cut_618},
    {44, {",,bxyxyxy  a", 12}, 3, jn_cut_619},
    {44, {",,xy a,a  ,,", 12}, 6, jn_cut_620},
    {44, {",,xy,,,,, axy", 13}, 8, jn_cut_621},
    {44, {",,xy,,b,", 8}, 6, jn_cut_622},
    {44, {",,xy,xy,xy,", 11}, 6, jn_cut_623},
    {44, {",b", 2}, 2, jn_cut_624},
    {44, {",b ,,  b", 8}, 4, jn_cut_625},
    {44, {",b,xyxy", 7}, 3, jn_cut_626},
    {44, {",xy", 3}, 2, jn_cut_627},
    {44, {",xybb", 5}, 2, jn_cut_628},
    {44, {"a b,ab ", 7}, 2, jn_cut_629},
    {44, {"a ba", 4}, 1, jn_cut_630},
    {44, {"a xy b", 6}, 1, jn_cut_631},
    {44, {"a,,, xyb ", 9}, 4, jn_cut_632},
    {44, {"a,,,,,", 6}, 6, jn_cut_633},
    {44, {"a,,xya,,,xy,,b", 14}, 8, jn_cut_634},
    {44, {"aa, a", 5}, 2, jn_cut_635},
    {44, {"aa,ba", 5}, 2, jn_cut_636},
    {44, {"aaa", 3}, 1, jn_cut_637},
    {44, {"aabab,", 6}, 2, jn_cut_638},
    {44, {"ab ,", 4}, 2, jn_cut_639},
    {44, {"ab,,,, , ", 9}, 6, jn_cut_640},
    {44, {"b", 1}, 1, jn_cut_641},
    {44, {"b  bbxy,,", 9}, 3, jn_cut_642},
    {44, {"b b", 3}, 1, jn_cut_643},
    {44, {"b b,,", 5}, 3, jn_cut_644},
    {44, {"b xyxyxy,,,,", 12}, 5, jn_cut_645},
    {44, {"b, xyxyxyxyaxy", 14}, 2, jn_cut_646},
    {44, {"b,,", 3}, 3, jn_cut_647},
    {44, {"b,,,,,,bb ab", 12}, 7, jn_cut_648},
    {44, {"b,,,,b,, xybxy", 14}, 7, jn_cut_649},
    {44, {"b,,a,,,bxyxyxy", 14}, 6, jn_cut_650},
    {44, {"b,,xy,,,,,,", 11}, 9, jn_cut_651},
    {44, {"b,,xy,,xyb ", 11}, 5, jn_cut_652},
    {44, {"b,a ,a,,xy a", 12}, 5, jn_cut_653},
    {44, {"b,xyxyxy,, ,", 12}, 5, jn_cut_654},
    {44, {"ba", 2}, 1, jn_cut_655},
    {44, {"ba,,, ", 6}, 4, jn_cut_656},
    {44, {"ba,axy,,", 8}, 4, jn_cut_657},
    {44, {"baaa,axy,", 9}, 3, jn_cut_658},
    {44, {"baab", 4}, 1, jn_cut_659},
    {44, {"bb,,xy,,", 8}, 5, jn_cut_660},
    {44, {"bbb", 3}, 1, jn_cut_661},
    {44, {"bxyb,a", 6}, 2, jn_cut_662},
    {44, {"xy", 2}, 1, jn_cut_663},
    {44, {"xy ,", 4}, 2, jn_cut_664},
    {44, {"xy, a", 5}, 2, jn_cut_665},
    {44, {"xy, bb,xy", 9}, 3, jn_cut_666},
    {44, {"xy,, baaxya ,,", 14}, 5, jn_cut_667},
    {44, {"xy,,,  a", 8}, 4, jn_cut_668},
    {44, {"xy,,,  axy,", 11}, 5, jn_cut_669},
    {44, {"xy,,,,,xy", 9}, 6, jn_cut_670},
    {44, {"xy,,,b,,,xy,", 12}, 8, jn_cut_671},
    {44, {"xy,,,xyba,,,,b", 14}, 8, jn_cut_672},
    {44, {"xy,,a,,a", 8}, 5, jn_cut_673},
    {44, {"xy,,aa", 6}, 3, jn_cut_674},
    {44, {"xy,,bbxy", 8}, 3, jn_cut_675},
    {44, {"xy,aa", 5}, 2, jn_cut_676},
    {44, {"xy,b", 4}, 2, jn_cut_677},
    {44, {"xy,b,, a,,a,,", 13}, 8, jn_cut_678},
    {44, {"xy,ba,a", 7}, 3, jn_cut_679},
    {44, {"xy,xy,,a", 8}, 4, jn_cut_680},
    {44, {"xya ,,b,,,,xy", 13}, 7, jn_cut_681},
    {44, {"xya b,,a, b", 11}, 4, jn_cut_682},
    {44, {"xya,", 4}, 2, jn_cut_683},
    {44, {"xyaxy a,,  ,,a", 14}, 5, jn_cut_684},
    {44, {"xyaxyb", 6}, 1, jn_cut_685},
    {44, {"xyb", 3}, 1, jn_cut_686},
    {44, {"xyb aa,,a", 9}, 3, jn_cut_687},
    {44, {"xyba,xy a,", 10}, 3, jn_cut_688},
    {44, {"xybb", 4}, 1, jn_cut_689},
    {44, {"xybxyb ", 7}, 1, jn_cut_690},
    {44, {"xyxy, ,,, ,", 11}, 6, jn_cut_691},
    {44, {"xyxy,,,,", 8}, 5, jn_cut_692},
    {44, {"xyxy,,,b,,,a, ", 14}, 8, jn_cut_693},
    {32, {"", 0}, 0, 0},
    {32, {" ", 1}, 2, jn_cut_695},
    {32, {"  ", 2}, 3, jn_cut_696},
    {32, {"a", 1}, 1, jn_cut_697},
    {32, {"a ", 2}, 2, jn_cut_698},
    {32, {" a", 2}, 2, jn_cut_699},
    {32, {"a b", 3}, 2, jn_cut_700},
    {32, {"a  b", 4}, 3, jn_cut_701},
    {32, {"\011", 1}, 1, jn_cut_702},
    {32, {"\011\011\011a", 4}, 1, jn_cut_703},
    {32, {"\011\011 \011bxy\011 ", 9}, 3, jn_cut_704},
    {32, {"\011\011 ba xy    ", 12}, 7, jn_cut_705},
    {32, {"\011   ", 4}, 4, jn_cut_706},
    {32, {"\011  xy\011\011 b\011", 10}, 4, jn_cut_707},
    {32, {"\011  xyaxybb", 10}, 3, jn_cut_708},
    {32, {"\011a    axy  ", 11}, 7, jn_cut_709},
    {32, {"\011aba", 4}, 1, jn_cut_710},
    {32, {"\011abb", 4}, 1, jn_cut_711},
    {32, {"\011bb  xya", 8}, 3, jn_cut_712},
    {32, {"\011xy   bxyb\011 ", 12}, 5, jn_cut_713},
    {32, {"\011xy  b a", 8}, 4, jn_cut_714},
    {32, {"\011xya  \011\011a  ", 11}, 5, jn_cut_715},
    {32, {"\011xyxy bxy   b", 13}, 5, jn_cut_716},
    {32, {" \011\011  b", 6}, 4, jn_cut_717},
    {32, {" \011xya", 5}, 2, jn_cut_718},
    {32, {"  \011  ba\011    a", 13}, 9, jn_cut_719},
    {32, {"  \011xyxyb  xyb", 13}, 5, jn_cut_720},
    {32, {"    ", 4}, 5, jn_cut_721},
    {32, {"      \011axya ", 12}, 8, jn_cut_722},
    {32, {"     axyb", 9}, 6, jn_cut_723},
    {32, {"    b   ", 8}, 8, jn_cut_724},
    {32, {"    b  a xya  ", 14}, 10, jn_cut_725},
    {32, {"    xy   xyxy", 13}, 8, jn_cut_726},
    {32, {"    xyxyxybxybxy", 16}, 5, jn_cut_727},
    {32, {"   a    bxyaxy", 14}, 8, jn_cut_728},
    {32, {"   ab  ", 7}, 6, jn_cut_729},
    {32, {"   b xy", 7}, 5, jn_cut_730},
    {32, {"   xyxyaxy ", 11}, 5, jn_cut_731},
    {32, {"  a", 3}, 3, jn_cut_732},
    {32, {"  a   \011", 7}, 6, jn_cut_733},
    {32, {"  a b ", 6}, 5, jn_cut_734},
    {32, {"  axyxy \011", 9}, 4, jn_cut_735},
    {32, {"  b\011\011  xyaa\011", 12}, 5, jn_cut_736},
    {32, {"  b  baxy\011   ", 13}, 8, jn_cut_737},
    {32, {"  bab", 5}, 3, jn_cut_738},
    {32, {"  bxy\011\011 a\011", 10}, 4, jn_cut_739},
    {32, {"  bxyxy", 7}, 3, jn_cut_740},
    {32, {"  xyaxy \011bb", 11}, 4, jn_cut_741},
    {32, {"  xyb", 5}, 3, jn_cut_742},
    {32, {"  xyxy  b  \011\011 ", 14}, 8, jn_cut_743},
    {32, {" a\011", 3}, 2, jn_cut_744},
    {32, {" a\011  aba", 8}, 4, jn_cut_745},
    {32, {" a  \011bxy", 8}, 4, jn_cut_746},
    {32, {" a    xy", 8}, 6, jn_cut_747},
    {32, {" b \011 bbxy xy", 12}, 5, jn_cut_748},
    {32, {" b  axy", 7}, 4, jn_cut_749},
    {32, {" ba a      b", 12}, 9, jn_cut_750},
    {32, {" bba  \011", 7}, 4, jn_cut_751},
    {32, {" xy    \011 ", 9}, 7, jn_cut_752},
    {32, {" xy xybxy", 9}, 3, jn_cut_753},
    {32, {" xya\011xy", 7}, 2, jn_cut_754},
    {32, {"a\011", 2}, 1, jn_cut_755},
    {32, {"a\011   ", 5}, 4, jn_cut_756},
    {32, {"a\011    ", 6}, 5, jn_cut_757},
    {32, {"a\011xybbb", 7}, 1, jn_cut_758},
    {32, {"a\011xybbxy\011b\011", 11}, 1, jn_cut_759},
    {32, {"a\011xybxy  \011", 10}, 3, jn_cut_760},
    {32, {"a  ", 3}, 3, jn_cut_761},
    {32, {"a  \011\011xyb", 8}, 3, jn_cut_762},
    {32, {"a    aaaa ", 10}, 6, jn_cut_763},
    {32, {"a    xy\011 a\011", 11}, 6, jn_cut_764},
    {32, {"a  a \011a \011\011", 10}, 5, jn_cut_765},
    {32, {"a  axyxyxy xya", 14}, 4, jn_cut_766},
    {32, {"a aaa\011", 6}, 2, jn_cut_767},
    {32, {"a xyaxyba xy\011", 13}, 3, jn_cut_768},
    {32, {"aa", 2}, 1, jn_cut_769},
    {32, {"aa  xyb", 7}, 3, jn_cut_770},
    {32, {"aab  \011  \011b", 10}, 5, jn_cut_771},
    {32, {"ab\011", 3}, 1, jn_cut_772},
    {32, {"ab\011bxy", 6}, 1, jn_cut_773},
    {32, {"aba  abb\011xya", 12}, 3, jn_cut_774},
    {32, {"abbb   xy  ", 11}, 6, jn_cut_775},
    {32, {"abxya  ", 7}, 3, jn_cut_776},
    {32, {"axy ", 4}, 2, jn_cut_777},
    {32, {"axy  \011", 6}, 3, jn_cut_778},
    {32, {"axy  xy", 7}, 3, jn_cut_779},
    {32, {"axy a a", 7}, 3, jn_cut_780},
    {32, {"axy xy  a ", 10}, 5, jn_cut_781},
    {32, {"axyb", 4}, 1, jn_cut_782},
    {32, {"axyb\011   ", 8}, 4, jn_cut_783},
    {32, {"axyxy    ", 9}, 5, jn_cut_784},
    {32, {"b", 1}, 1, jn_cut_785},
    {32, {"b\011   xy\011\011", 9}, 4, jn_cut_786},
    {32, {"b\011xyxyb  xy \011xy", 15}, 4, jn_cut_787},
    {32, {"b ", 2}, 2, jn_cut_788},
    {32, {"b \011\011", 4}, 2, jn_cut_789},
    {32, {"b  ", 3}, 3, jn_cut_790},
    {32, {"b  \011\011", 5}, 3, jn_cut_791},
    {32, {"b          b", 12}, 11, jn_cut_792},
    {32, {"b   b xy ", 9}, 6, jn_cut_793},
    {32, {"ba", 2}, 1, jn_cut_794},
    {32, {"ba\011  \011b ba", 10}, 4, jn_cut_795},
    {32, {"ba  ", 4}, 3, jn_cut_796},
    {32, {"baxyxy", 6}, 1, jn_cut_797},
    {32, {"bb", 2}, 1, jn_cut_798},
    {32, {"bb\011  ", 5}, 3, jn_cut_799},
    {32, {"bb  ", 4}, 3, jn_cut_800},
    {32, {"bxy    xyaa\011", 12}, 5, jn_cut_801},
    {32, {"bxya", 4}, 1, jn_cut_802},
    {32, {"bxyxy", 5}, 1, jn_cut_803},
    {32, {"xy", 2}, 1, jn_cut_804},
    {32, {"xy\011ab  xyxy ", 12}, 4, jn_cut_805},
    {32, {"xy   ", 5}, 4, jn_cut_806},
    {32, {"xy  a ", 6}, 4, jn_cut_807},
    {32, {"xy  b", 5}, 3, jn_cut_808},
    {32, {"xyaaa  ", 7}, 3, jn_cut_809},
    {32, {"xyaxyaab a ", 11}, 3, jn_cut_810},
    {32, {"xyb", 3}, 1, jn_cut_811},
    {32, {"xybb xyb  xyb", 13}, 4, jn_cut_812},
    {32, {"xyxy  aaxy", 10}, 3, jn_cut_813},
    {32, {"xyxyxy a", 8}, 2, jn_cut_814},
    {32, {"xyxyxya  ", 9}, 3, jn_cut_815},
    {9, {"", 0}, 0, 0},
    {9, {"\011", 1}, 2, jn_cut_817},
    {9, {"\011\011", 2}, 3, jn_cut_818},
    {9, {"a", 1}, 1, jn_cut_819},
    {9, {"a\011", 2}, 2, jn_cut_820},
    {9, {"\011a", 2}, 2, jn_cut_821},
    {9, {"a\011b", 3}, 2, jn_cut_822},
    {9, {"a\011\011b", 4}, 3, jn_cut_823},
    {9, {"\011\011\011\011 b\011b  ", 10}, 6, jn_cut_824},
    {9, {"\011\011\011bbxya\011", 9}, 5, jn_cut_825},
    {9, {"\011\011 xyxy\011 xy ", 12}, 4, jn_cut_826},
    {9, {"\011\011axybxy", 8}, 3, jn_cut_827},
    {9, {"\011\011bbxy\011\011", 8}, 5, jn_cut_828},
    {9, {"\011\011xy\011bbba", 9}, 4, jn_cut_829},
    {9, {"\011  \011aba\011 ", 9}, 4, jn_cut_830},
    {9, {"\011  b\011", 5}, 3, jn_cut_831},
    {9, {"\011 a ", 4}, 2, jn_cut_832},
    {9, {"\011 axyxybb  ", 11}, 2, jn_cut_833},
    {9, {"\011a\011 aa aa ", 10}, 3, jn_cut_834},
    {9, {"\011aaaxyxyaxy", 11}, 2, jn_cut_835},
    {9, {"\011axy\011 xy", 8}, 3, jn_cut_836},
    {9, {"\011axya", 5}, 2, jn_cut_837},
    {9, {"\011b \011\011 a", 7}, 4, jn_cut_838},
    {9, {"\011b   xy", 7}, 2, jn_cut_839},
    {9, {"\011ba\011xyaaxy", 10}, 3, jn_cut_840},
    {9, {"\011bab \011b", 7}, 3, jn_cut_841},
    {9, {"\011bxy\011xy\011", 8}, 4, jn_cut_842},
    {9, {"\011bxyabbba b", 11}, 2, jn_cut_843},
    {9, {"\011bxybaxy", 8}, 2, jn_cut_844},
    {9, {"\011xy\011\011a", 6}, 4, jn_cut_845},
    {9, {"\011xy b b\011\011 \011", 11}, 5, jn_cut_846},
    {9, {"\011xyb b\011xy a", 11}, 3, jn_cut_847},
    {9, {" ", 1}, 1, jn_cut_848},
    {9, {" \011", 2}, 2, jn_cut_849},
    {9, {" \011ab ab ", 8}, 2, jn_cut_850},
    {9, {" \011b", 3}, 2, jn_cut_851},
    {9, {" \011bb   axy", 10}, 2, jn_cut_852},
    {9, {" \011xy\011 \011 xy\011\011", 12}, 6, jn_cut_853},
    {9, {"  a", 3}, 1, jn_cut_854},
    {9, {"  abxyxyabb", 11}, 1, jn_cut_855},
    {9, {" a\011", 3}, 2, jn_cut_856},
    {9, {" a\011\011 b b", 8}, 3, jn_cut_857},
    {9, {" a\011a", 4}, 2, jn_cut_858},
    {9, {" a ", 3}, 1, jn_cut_859},
    {9, {" a a a  ", 8}, 1, jn_cut_860},
    {9, {" a xy \011xy\011a\011", 12}, 4, jn_cut_861},
    {9, {" aa\011 b\011", 7}, 3, jn_cut_862},
    {9, {" abbxy\011\011 ", 9}, 3, jn_cut_863},
    {9, {" axy", 4}, 1, jn_cut_864},
    {9, {" b ", 3}, 1, jn_cut_865},
    {9, {" ba", 3}, 1, jn_cut_866},
    {9, {" baxy xyb", 9}, 1, jn_cut_867},
    {9, {" bxyaaa a", 9}, 1, jn_cut_868},
    {9, {" xybb", 5}, 1, jn_cut_869},
    {9, {" xybb\011xyxy", 10}, 2, jn_cut_870},
    {9, {"a\011\011\011abbab", 9}, 4, jn_cut_871},
    {9, {"a\011\011bab ", 7}, 3, jn_cut_872},
    {9, {"a\011 ", 3}, 2, jn_cut_873},
    {9, {"a\011 xy", 5}, 2, jn_cut_874},
    {9, {"a  xy ", 6}, 1, jn_cut_875},
    {9, {"a  xyxya\011 b", 11}, 2, jn_cut_876},
    {9, {"aa", 2}, 1, jn_cut_877},
    {9, {"aa \011b", 5}, 2, jn_cut_878},
    {9, {"aa  \011xyxy", 9}, 2, jn_cut_879},
    {9, {"aaa", 3}, 1, jn_cut_880},
    {9, {"aaa bxya", 8}, 1, jn_cut_881},
    {9, {"aaaa ", 5}, 1, jn_cut_882},
    {9, {"aab \011\011", 6}, 3, jn_cut_883},
    {9, {"aaxy\011xyxya\011b ", 13}, 3, jn_cut_884},
    {9, {"aaxy bxy", 8}, 1, jn_cut_885},
    {9, {"ab ", 3}, 1, jn_cut_886},
    {9, {"abab\011xy xy a", 12}, 2, jn_cut_887},
    {9, {"abbxy", 5}, 1, jn_cut_888},
    {9, {"abxy", 4}, 1, jn_cut_889},
    {9, {"abxyxyb", 7}, 1, jn_cut_890},
    {9, {"axy\011bb  bbxy", 12}, 2, jn_cut_891},
    {9, {"axy \011\011", 6}, 3, jn_cut_892},
    {9, {"b", 1}, 1, jn_cut_893},
    {9, {"b\011 aaa", 6}, 2, jn_cut_894},
    {9, {"b\011 xy", 5}, 2, jn_cut_895},
    {9, {"b\011xyxy", 6}, 2, jn_cut_896},
    {9, {"b ", 2}, 1, jn_cut_897},
    {9, {"b   a \011", 7}, 2, jn_cut_898},
    {9, {"b xy xy \011 \011xy", 13}, 3, jn_cut_899},
    {9, {"b xya xy", 8}, 1, jn_cut_900},
    {9, {"ba  bxyxy\011ba", 12}, 2, jn_cut_901},
    {9, {"baaaxy", 6}, 1, jn_cut_902},
    {9, {"baxy", 4}, 1, jn_cut_903},
    {9, {"baxya b\011\011", 9}, 3, jn_cut_904},
    {9, {"baxyb", 5}, 1, jn_cut_905},
    {9, {"bb", 2}, 1, jn_cut_906},
    {9, {"bb\011xy", 5}, 2, jn_cut_907},
    {9, {"bb xy    xyb", 12}, 1, jn_cut_908},
    {9, {"bba a\011b\011bxy", 11}, 3, jn_cut_909},
    {9, {"bxy", 3}, 1, jn_cut_910},
    {9, {"bxy\011\011xy\011xy", 10}, 4, jn_cut_911},
    {9, {"bxy\011baxya", 9}, 2, jn_cut_912},
    {9, {"bxy axyb \011", 10}, 2, jn_cut_913},
    {9, {"bxyaaxy b\011", 10}, 2, jn_cut_914},
    {9, {"bxyb\011", 5}, 2, jn_cut_915},
    {9, {"bxyxyxyxy", 9}, 1, jn_cut_916},
    {9, {"xy", 2}, 1, jn_cut_917},
    {9, {"xy\011", 3}, 2, jn_cut_918},
    {9, {"xy\011\011ba    ", 10}, 3, jn_cut_919},
    {9, {"xy\011 bxy\011b  ", 11}, 3, jn_cut_920},
    {9, {"xy ", 3}, 1, jn_cut_921},
    {9, {"xy \011b a", 7}, 2, jn_cut_922},
    {9, {"xy  xyxy b\011 ", 12}, 2, jn_cut_923},
    {9, {"xy a\011   \011bxy", 12}, 3, jn_cut_924},
    {9, {"xy aa\011", 6}, 2, jn_cut_925},
    {9, {"xy xy\011 \011xy bb", 13}, 3, jn_cut_926},
    {9, {"xya \011\011a", 7}, 3, jn_cut_927},
    {9, {"xyaab", 5}, 1, jn_cut_928},
    {9, {"xyaabb", 6}, 1, jn_cut_929},
    {9, {"xyaaxy baxy", 11}, 1, jn_cut_930},
    {9, {"xyb\011bab\011 ab", 11}, 3, jn_cut_931},
    {9, {"xyba \011b", 7}, 2, jn_cut_932},
    {9, {"xybaaxybxyxya", 13}, 1, jn_cut_933},
    {9, {"xybbxyxy\011 xya\011", 14}, 3, jn_cut_934},
    {9, {"xyxy", 4}, 1, jn_cut_935},
    {9, {"xyxy\011 ", 6}, 2, jn_cut_936},
    {9, {"xyxy\011 xy  ", 10}, 2, jn_cut_937},
    {9, {"xyxy\011b a", 8}, 2, jn_cut_938},
    {9, {"xyxya", 5}, 1, jn_cut_939},
    {97, {"", 0}, 0, 0},
    {97, {"a", 1}, 2, jn_cut_941},
    {97, {"aa", 2}, 3, jn_cut_942},
    {97, {"a", 1}, 2, jn_cut_943},
    {97, {"aa", 2}, 3, jn_cut_944},
    {97, {"aa", 2}, 3, jn_cut_945},
    {97, {"aab", 3}, 3, jn_cut_946},
    {97, {"aaab", 4}, 4, jn_cut_947},
    {97, {" ", 1}, 1, jn_cut_948},
    {97, {"  ", 2}, 1, jn_cut_949},
    {97, {"   aaaaaacab ", 13}, 8, jn_cut_950},
    {97, {"  a b", 5}, 2, jn_cut_951},
    {97, {"  aa abaaaca", 12}, 8, jn_cut_952},
    {97, {"  aac bbcc", 10}, 3, jn_cut_953},
    {97, {" a", 2}, 2, jn_cut_954},
    {97, {" aa", 3}, 3, jn_cut_955},
    {97, {" aa aa ab", 9}, 6, jn_cut_956},
    {97, {" aa aaabb ", 10}, 6, jn_cut_957},
    {97, {" aaaa", 5}, 5, jn_cut_958},
    {97, {" aaaa aa  a", 11}, 8, jn_cut_959},
    {97, {" aaaaccaaa", 10}, 8, jn_cut_960},
    {97, {" aab aa", 7}, 5, jn_cut_961},
    {97, {" aac", 4}, 3, jn_cut_962},
    {97, {" abb a  aa", 10}, 5, jn_cut_963},
    {97, {" abbaaaaa", 9}, 7, jn_cut_964},
    {97, {" ba ba c ", 9}, 3, jn_cut_965},
    {97, {" baaa", 5}, 4, jn_cut_966},
    {97, {" baaa aab", 9}, 6, jn_cut_967},
    {97, {" baaaaaaaaa", 11}, 10, jn_cut_968},
    {97, {" baaaabaaac ", 12}, 8, jn_cut_969},
    {97, {" baabaa ", 8}, 5, jn_cut_970},
    {97, {" bc", 3}, 1, jn_cut_971},
    {97, {" bcb", 4}, 1, jn_cut_972},
    {97, {" c c", 4}, 1, jn_cut_973},
    {97, {"a   b", 5}, 2, jn_cut_974},
    {97, {"a  ac", 5}, 3, jn_cut_975},
    {97, {"a b  bbc ", 9}, 2, jn_cut_976},
    {97, {"aa a", 4}, 4, jn_cut_977},
    {97, {"aa aacacc ", 10}, 6, jn_cut_978},
    {97, {"aa b ccb a ", 11}, 4, jn_cut_979},
    {97, {"aa baaa  ", 9}, 6, jn_cut_980},
    {97, {"aaa aaaa b", 10}, 8, jn_cut_981},
    {97, {"aaaa", 4}, 5, jn_cut_982},
    {97, {"aaaa bcb", 8}, 5, jn_cut_983},
    {97, {"aaaa cbcaaa", 11}, 8, jn_cut_984},
    {97, {"aaaaa acaaa", 11}, 10, jn_cut_985},
    {97, {"aaaaa caacaaab", 14}, 11, jn_cut_986},
    {97, {"aaaaacaabbaaaaa", 15}, 13, jn_cut_987},
    {97, {"aaaabcabaaa", 11}, 9, jn_cut_988},
    {97, {"aaaacb", 6}, 5, jn_cut_989},
    {97, {"aaab ", 5}, 4, jn_cut_990},
    {97, {"aaaba", 5}, 5, jn_cut_991},
    {97, {"aaabb aa ", 9}, 6, jn_cut_992},
    {97, {"aaaca c cab", 11}, 6, jn_cut_993},
    {97, {"aaacb", 5}, 4, jn_cut_994},
    {97, {"aaacbbaac", 9}, 6, jn_cut_995},
    {97, {"aab aabbaa ", 11}, 7, jn_cut_996},
    {97, {"aab c", 5}, 3, jn_cut_997},
    {97, {"aabaa", 5}, 5, jn_cut_998},
    {97, {"aabbc aa", 8}, 5, jn_cut_999},
    {97, {"aabbcc ", 7}, 3, jn_cut_1000},
    {97, {"aabc cab", 8}, 4, jn_cut_1001},
    {97, {"aacacccb", 8}, 4, jn_cut_1002},
    {97, {"aaccbab c ", 10}, 4, jn_cut_1003},
    {97, {"ab", 2}, 2, jn_cut_1004},
    {97, {"abaaaaaaaaaa c", 14}, 12, jn_cut_1005},
    {97, {"ababaa", 6}, 5, jn_cut_1006},
    {97, {"abb ", 4}, 2, jn_cut_1007},
    {97, {"ac", 2}, 2, jn_cut_1008},
    {97, {"aca baa", 7}, 5, jn_cut_1009},
    {97, {"acaa a", 6}, 5, jn_cut_1010},
    {97, {"acaa bcaaaa ", 12}, 8, jn_cut_1011},
    {97, {"acb aa", 6}, 4, jn_cut_1012},
    {97, {"acbac  c", 8}, 3, jn_cut_1013},
    {97, {"b", 1}, 1, jn_cut_1014},
    {97, {"b aabcaaa c", 11}, 6, jn_cut_1015},
    {97, {"b baabcaa a", 11}, 6, jn_cut_1016},
    {97, {"b bb", 4}, 1, jn_cut_1017},
    {97, {"b bbaab aa ", 11}, 5, jn_cut_1018},
    {97, {"b bcaa", 6}, 3, jn_cut_1019},
    {97, {"ba", 2}, 2, jn_cut_1020},
    {97, {"ba bcaaaaabc", 12}, 7, jn_cut_1021},
    {97, {"baa", 3}, 3, jn_cut_1022},
    {97, {"baa  b", 6}, 3, jn_cut_1023},
    {97, {"baaacbaaa", 9}, 7, jn_cut_1024},
    {97, {"baac ", 5}, 3, jn_cut_1025},
    {97, {"baac cbbaa aa", 13}, 7, jn_cut_1026},
    {97, {"baacaca", 7}, 5, jn_cut_1027},
    {97, {"baca  bca", 9}, 4, jn_cut_1028},
    {97, {"bba", 3}, 2, jn_cut_1029},
    {97, {"bbaaaabaab", 10}, 7, jn_cut_1030},
    {97, {"bbaaca", 6}, 4, jn_cut_1031},
    {97, {"bbbaaa", 6}, 4, jn_cut_1032},
    {97, {"bbbbacaa", 8}, 4, jn_cut_1033},
    {97, {"bc ba  aa", 9}, 4, jn_cut_1034},
    {97, {"bc ca  c", 8}, 2, jn_cut_1035},
    {97, {"bcaaaacaa baa", 13}, 9, jn_cut_1036},
    {97, {"bcbb ", 5}, 1, jn_cut_1037},
    {97, {"bcc", 3}, 1, jn_cut_1038},
    {97, {"c", 1}, 1, jn_cut_1039},
    {97, {"c ", 2}, 1, jn_cut_1040},
    {97, {"c aa ", 5}, 3, jn_cut_1041},
    {97, {"c aaaaccbbcaa", 13}, 7, jn_cut_1042},
    {97, {"c baa", 5}, 3, jn_cut_1043},
    {97, {"c cc", 4}, 1, jn_cut_1044},
    {97, {"ca a", 4}, 3, jn_cut_1045},
    {97, {"ca aaac aab ", 12}, 7, jn_cut_1046},
    {97, {"caa", 3}, 3, jn_cut_1047},
    {97, {"caa ac", 6}, 4, jn_cut_1048},
    {97, {"caaa", 4}, 4, jn_cut_1049},
    {97, {"caaaaa", 6}, 6, jn_cut_1050},
    {97, {"caaaaaab", 8}, 7, jn_cut_1051},
    {97, {"caab aaa ", 9}, 6, jn_cut_1052},
    {97, {"caabbaa b", 9}, 5, jn_cut_1053},
    {97, {"caac b aaaa", 11}, 7, jn_cut_1054},
    {97, {"caacb  ", 7}, 3, jn_cut_1055},
    {97, {"cb abbaa", 8}, 4, jn_cut_1056},
    {97, {"cbaaaa  ", 8}, 5, jn_cut_1057},
    {97, {"cbaaaaaa", 8}, 7, jn_cut_1058},
    {97, {"cbb", 3}, 1, jn_cut_1059},
    {97, {"cbc aacaaaab", 12}, 7, jn_cut_1060},
    {97, {"cc", 2}, 1, jn_cut_1061},
    {97, {"ccbabaaa", 8}, 5, jn_cut_1062},
    {97, {"ccbbbaacaa", 10}, 5, jn_cut_1063},
    {97, {"ccccb", 5}, 1, jn_cut_1064},
    {0, {"", 0}, 0, 0},
    {0, {"\000", 1}, 2, jn_cut_1066},
    {0, {"\000\000", 2}, 3, jn_cut_1067},
    {0, {"a", 1}, 1, jn_cut_1068},
    {0, {"a\000", 2}, 2, jn_cut_1069},
    {0, {"\000a", 2}, 2, jn_cut_1070},
    {0, {"a\000b", 3}, 2, jn_cut_1071},
    {0, {"a\000\000b", 4}, 3, jn_cut_1072},
    {0, {"\000\000\000\000", 4}, 5, jn_cut_1073},
    {0, {"\000\000\000\000\000\000\000\000\000 \000\000\000\000\000", 15}, 15, jn_cut_1074},
    {0, {"\000\000\000\000\000\000\000\000a", 9}, 9, jn_cut_1075},
    {0, {"\000\000\000\000\000\000\000b\000\000", 10}, 10, jn_cut_1076},
    {0, {"\000\000\000\000\000\000\000b\000\000aa\000\000", 14}, 12, jn_cut_1077},
    {0, {"\000\000\000\000\000 a", 7}, 6, jn_cut_1078},
    {0, {"\000\000\000\000 b\000\000", 8}, 7, jn_cut_1079},
    {0, {"\000\000\000\000 bb\000\000", 9}, 7, jn_cut_1080},
    {0, {"\000\000\000\000a\000\000", 7}, 7, jn_cut_1081},
    {0, {"\000\000\000\000a \000\000 ", 9}, 7, jn_cut_1082},
    {0, {"\000\000\000\000b a", 7}, 5, jn_cut_1083},
    {0, {"\000\000\000\000bb\000\000b\000\000", 11}, 9, jn_cut_1084},
    {0, {"\000\000\000 ", 4}, 4, jn_cut_1085},
    {0, {"\000\000\000 \000\000\000", 7}, 7, jn_cut_1086},
    {0, {"\000\000\000 a\000a\000a\000\000", 11}, 8, jn_cut_1087},
    {0, {"\000\000\000b", 4}, 4, jn_cut_1088},
    {0, {"\000\000\000b\000\000\000", 7}, 7, jn_cut_1089},
    {0, {"\000\000\000b\000\000 \000", 8}, 7, jn_cut_1090},
    {0, {"\000\000\000ba b \000\000", 10}, 6, jn_cut_1091},
    {0, {"\000\000 \000\000\000 ", 7}, 6, jn_cut_1092},
    {0, {"\000\000 \000\000  ", 7}, 5, jn_cut_1093},
    {0, {"\000\000 \000a", 5}, 4, jn_cut_1094},
    {0, {"\000\000 a\000\000\000", 7}, 6, jn_cut_1095},
    {0, {"\000\000 aa\000 baa", 10}, 4, jn_cut_1096},
    {0, {"\000\000 b", 4}, 3, jn_cut_1097},
    {0, {"\000\000 b   b\000\000", 10}, 5, jn_cut_1098},
    {0, {"\000\000 b b", 6}, 3, jn_cut_1099},
    {0, {"\000\000 b b\000\000", 8}, 5, jn_cut_1100},
    {0, {"\000\000 bab\000\000ab", 10}, 5, jn_cut_1101},
    {0, {"\000\000a\000\000", 5}, 5, jn_cut_1102},
    {0, {"\000\000a\000\000 ", 6}, 5, jn_cut_1103},
    {0, {"\000\000a aa", 6}, 3, jn_cut_1104},
    {0, {"\000\000aa\000\000aa \000\000b\000", 13}, 8, jn_cut_1105},
    {0, {"\000\000ab\000 \000\000\000b", 10}, 7, jn_cut_1106},
    {0, {"\000\000abbbaaa", 9}, 3, jn_cut_1107},
    {0, {"\000\000b", 3}, 3, jn_cut_1108},
    {0, {"\000\000b\000\000b\000b ", 9}, 6, jn_cut_1109},
    {0, {"\000\000b\000b\000\000 a\000\000\000", 12}, 9, jn_cut_1110},
    {0, {"\000\000b a\000\000\000 ", 9}, 6, jn_cut_1111},
    {0, {"\000\000bb\000\000  b\000 ", 11}, 6, jn_cut_1112},
    {0, {"\000 \000\000\000\000", 6}, 6, jn_cut_1113},
    {0, {"\000 \000\000\000b a\000b\000\000", 12}, 8, jn_cut_1114},
    {0, {"\000 \000\000 b", 6}, 4, jn_cut_1115},
    {0, {"\000  \000\000\000\000\000bb", 10}, 7, jn_cut_1116},
    {0, {"\000  \000 ", 5}, 3, jn_cut_1117},
    {0, {"\000a\000", 3}, 3, jn_cut_1118},
    {0, {"\000a\000\000", 4}, 4, jn_cut_1119},
    {0, {"\000a bb\000\000\000", 8}, 5, jn_cut_1120},
    {0, {"\000aa \000\000", 6}, 4, jn_cut_1121},
    {0, {" ", 1}, 1, jn_cut_1122},
    {0, {" \000\000\000\000\000\000\000", 8}, 8, jn_cut_1123},
    {0, {" \000\000\000\000 \000\000", 8}, 7, jn_cut_1124},
    {0, {" \000\000\000\000ab a \000\000", 12}, 7, jn_cut_1125},
    {0, {" \000\000\000 a\000\000\000b b", 12}, 7, jn_cut_1126},
    {0, {" \000\000\000a a", 7}, 4, jn_cut_1127},
    {0, {" \000\000\000aba", 7}, 4, jn_cut_1128},
    {0, {" \000\000  \000\000", 7}, 5, jn_cut_1129},
    {0, {" \000\000 b", 5}, 3, jn_cut_1130},
    {0, {" \000\000a\000", 5}, 4, jn_cut_1131},
    {0, {" \000\000b  ", 6}, 3, jn_cut_1132},
    {0, {" \000 aa\000\000\000\000", 9}, 6, jn_cut_1133},
    {0, {"  \000\000", 4}, 3, jn_cut_1134},
    {0, {"  \000\000\000 ", 6}, 4, jn_cut_1135},
    {0, {"   ba \000aa", 9}, 2, jn_cut_1136},
    {0, {"  ba\000\000  \000a", 10}, 4, jn_cut_1137},
    {0, {" a", 2}, 1, jn_cut_1138},
    {0, {" a b a\000b", 8}, 2, jn_cut_1139},
    {0, {" aba bb\000\000 ", 10}, 3, jn_cut_1140},
    {0, {" b", 2}, 1, jn_cut_1141},
    {0, {" b\000", 3}, 2, jn_cut_1142},
    {0, {" ba ", 4}, 1, jn_cut_1143},
    {0, {"a\000\000", 3}, 3, jn_cut_1144},
    {0, {"a\000\000\000b\000\000\000\000\000 b", 12}, 9, jn_cut_1145},
    {0, {"a\000\000 \000\000aa\000b\000a", 12}, 7, jn_cut_1146},
    {0, {"a\000\000 b\000 aab", 10}, 4, jn_cut_1147},
    {0, {"a\000\000a ", 5}, 3, jn_cut_1148},
    {0, {"a\000 ", 3}, 2, jn_cut_1149},
    {0, {"a\000a", 3}, 2, jn_cut_1150},
    {0, {"a\000aa \000\000b \000", 10}, 5, jn_cut_1151},
    {0, {"a\000bab", 5}, 2, jn_cut_1152},
    {0, {"a \000\000a\000\000aa", 9}, 5, jn_cut_1153},
    {0, {"a a \000\000\000", 7}, 4, jn_cut_1154},
    {0, {"a a aa\000 b ", 10}, 2, jn_cut_1155},
    {0, {"a bba", 5}, 1, jn_cut_1156},
    {0, {"aaa   aa", 8}, 1, jn_cut_1157},
    {0, {"aaaab ", 6}, 1, jn_cut_1158},
    {0, {"ab\000\000b\000\000\000\000\000\000\000\000", 13}, 11, jn_cut_1159},
    {0, {"ab\000b\000\000\000b", 8}, 5, jn_cut_1160},
    {0, {"aba ", 4}, 1, jn_cut_1161},
    {0, {"b", 1}, 1, jn_cut_1162},
    {0, {"b\000\000", 3}, 3, jn_cut_1163},
    {0, {"b\000\000\000\000\000a", 7}, 6, jn_cut_1164},
    {0, {"b\000\000   \000aa  ", 11}, 4, jn_cut_1165},
    {0, {"b\000\000ab\000\000", 7}, 5, jn_cut_1166},
    {0, {"b\000\000b\000aba\000\000\000", 11}, 7, jn_cut_1167},
    {0, {"b\000 a\000\000\000\000bb", 10}, 6, jn_cut_1168},
    {0, {"b\000a a\000\000bba", 10}, 4, jn_cut_1169},
    {0, {"b\000bbb", 5}, 2, jn_cut_1170},
    {0, {"b  \000\000\000  a", 9}, 4, jn_cut_1171},
    {0, {"b a", 3}, 1, jn_cut_1172},
    {0, {"b a  \000b", 7}, 2, jn_cut_1173},
    {0, {"b aa ", 5}, 1, jn_cut_1174},
    {0, {"ba\000\000\000\000", 6}, 5, jn_cut_1175},
    {0, {"ba\000\000b", 5}, 3, jn_cut_1176},
    {0, {"ba\000 bbbb", 8}, 2, jn_cut_1177},
    {0, {"ba\000a ", 5}, 2, jn_cut_1178},
    {0, {"ba\000bb \000\000", 8}, 4, jn_cut_1179},
    {0, {"ba aa\000\000 \000\000\000\000b", 13}, 7, jn_cut_1180},
    {0, {"bb\000\000a", 5}, 3, jn_cut_1181},
    {0, {"bb\000a ", 5}, 2, jn_cut_1182},
    {0, {"bb ab", 5}, 1, jn_cut_1183},
    {0, {"bba", 3}, 1, jn_cut_1184},
    {0, {"bbaa\000\000b\000\000 ", 10}, 5, jn_cut_1185},
    {0, {"bbaa \000\000", 7}, 3, jn_cut_1186},
    {233, {"", 0}, 0, 0},
    {233, {"\351", 1}, 2, jn_cut_1188},
    {233, {"\351\351", 2}, 3, jn_cut_1189},
    {233, {"a", 1}, 1, jn_cut_1190},
    {233, {"a\351", 2}, 2, jn_cut_1191},
    {233, {"\351a", 2}, 2, jn_cut_1192},
    {233, {"a\351b", 3}, 2, jn_cut_1193},
    {233, {"a\351\351b", 4}, 3, jn_cut_1194},
    {233, {" ", 1}, 1, jn_cut_1195},
    {233, {"   ", 3}, 1, jn_cut_1196},
    {233, {"   b \351aa\351 ", 10}, 3, jn_cut_1197},
    {233, {"  aba", 5}, 1, jn_cut_1198},
    {233, {"  \351ab a", 7}, 2, jn_cut_1199},
    {233, {" a", 2}, 1, jn_cut_1200},
    {233, {" a  ", 4}, 1, jn_cut_1201},
    {233, {" a b a", 6}, 1, jn_cut_1202},
    {233, {" aaa \351\351a\351", 9}, 4, jn_cut_1203},
    {233, {" aaba\351a ", 8}, 2, jn_cut_1204},
    {233, {" aa\351bab   ", 10}, 2, jn_cut_1205},
    {233, {" ab aba ", 8}, 1, jn_cut_1206},
    {233, {" a\351 a\351", 6}, 3, jn_cut_1207},
    {233, {" a\351 b", 5}, 2, jn_cut_1208},
    {233, {" a\351a\351\351a a\351", 10}, 5, jn_cut_1209},
    {233, {" a\351ba\351abb", 9}, 3, jn_cut_1210},
    {233, {" a\351bb", 5}, 2, jn_cut_1211},
    {233, {" a\351\351", 4}, 3, jn_cut_1212},
    {233, {" b b\351b", 6}, 2, jn_cut_1213},
    {233, {" b \351\351\351a ", 8}, 4, jn_cut_1214},
    {233, {" ba", 3}, 1, jn_cut_1215},
    {233, {" ba\351 ", 5}, 2, jn_cut_1216},
    {233, {" bba", 4}, 1, jn_cut_1217},
    {233, {" bbbb\351", 6}, 2, jn_cut_1218},
    {233, {" b\351\351a ab", 8}, 3, jn_cut_1219},
    {233, {" b\351\351baa", 7}, 3, jn_cut_1220},
    {233, {" \351 a\351", 5}, 3, jn_cut_1221},
    {233, {" \351 a\351\351bbb", 9}, 4, jn_cut_1222},
    {233, {" \351 \351 ", 5}, 3, jn_cut_1223},
    {233, {" \351ab", 4}, 2, jn_cut_1224},
    {233, {" \351b ", 4}, 2, jn_cut_1225},
    {233, {" \351b b baa", 9}, 2, jn_cut_1226},
    {233, {" \351bab \351b", 8}, 3, jn_cut_1227},
    {233, {"a  a\351", 5}, 2, jn_cut_1228},
    {233, {"a aa", 4}, 1, jn_cut_1229},
    {233, {"a a\351 a\351", 7}, 3, jn_cut_1230},
    {233, {"a a\351b a\351", 8}, 3, jn_cut_1231},
    {233, {"a baaab", 7}, 1, jn_cut_1232},
    {233, {"a baa\351a  b", 10}, 2, jn_cut_1233},
    {233, {"a bab\351", 6}, 2, jn_cut_1234},
    {233, {"a bbaa\351", 7}, 2, jn_cut_1235},
    {233, {"a b\351\351 \351a\351a", 10}, 5, jn_cut_1236},
    {233, {"a \351", 3}, 2, jn_cut_1237},
    {233, {"aa \351a\351", 6}, 3, jn_cut_1238},
    {233, {"aaa  a", 6}, 1, jn_cut_1239},
    {233, {"aaa ba\351", 7}, 2, jn_cut_1240},
    {233, {"aaa\351bb\351aa", 9}, 3, jn_cut_1241},
    {233, {"aaa\351\351aaa", 8}, 3, jn_cut_1242},
    {233, {"ab", 2}, 1, jn_cut_1243},
    {233, {"aba", 3}, 1, jn_cut_1244},
    {233, {"aba  \351 \351a", 9}, 3, jn_cut_1245},
    {233, {"aba\351ba", 6}, 2, jn_cut_1246},
    {233, {"abba a ", 7}, 1, jn_cut_1247},
    {233, {"abbab", 5}, 1, jn_cut_1248},
    {233, {"abb\351aa", 6}, 2, jn_cut_1249},
    {233, {"ab\351\351 bb", 7}, 3, jn_cut_1250},
    {233, {"a\351a ab ", 7}, 2, jn_cut_1251},
    {233, {"a\351aa\351a\351b ", 9}, 4, jn_cut_1252},
    {233, {"a\351bb\351ba", 7}, 3, jn_cut_1253},
    {233, {"a\351\351", 3}, 3, jn_cut_1254},
    {233, {"a\351\351aa b", 7}, 3, jn_cut_1255},
    {233, {"b", 1}, 1, jn_cut_1256},
    {233, {"b  b", 4}, 1, jn_cut_1257},
    {233, {"b aa b\351", 7}, 2, jn_cut_1258},
    {233, {"b ab ab", 7}, 1, jn_cut_1259},
    {233, {"b abab\351 \351b", 10}, 3, jn_cut_1260},
    {233, {"b a\351 \351b", 7}, 3, jn_cut_1261},
    {233, {"b ba\351\351", 6}, 3, jn_cut_1262},
    {233, {"b \351 \351 b\351a", 9}, 4, jn_cut_1263},
    {233, {"b \351\351", 4}, 3, jn_cut_1264},
    {233, {"b \351\351\351b ", 7}, 4, jn_cut_1265},
    {233, {"ba", 2}, 1, jn_cut_1266},
    {233, {"ba \351 ", 5}, 2, jn_cut_1267},
    {233, {"ba \351b", 5}, 2, jn_cut_1268},
    {233, {"baa", 3}, 1, jn_cut_1269},
    {233, {"baa ", 4}, 1, jn_cut_1270},
    {233, {"baa   ab b", 10}, 1, jn_cut_1271},
    {233, {"baa \351", 5}, 2, jn_cut_1272},
    {233, {"baaa \351ba  ", 10}, 2, jn_cut_1273},
    {233, {"babab\351\351\351", 8}, 4, jn_cut_1274},
    {233, {"ba\351a\351", 5}, 3, jn_cut_1275},
    {233, {"bb bb\351 b\351a", 10}, 3, jn_cut_1276},
    {233, {"bbab  ", 6}, 1, jn_cut_1277},
    {233, {"bba\351", 4}, 2, jn_cut_1278},
    {233, {"bbb", 3}, 1, jn_cut_1279},
    {233, {"bb\351 b  aba", 10}, 2, jn_cut_1280},
    {233, {"bb\351b\351 \351aaa", 10}, 4, jn_cut_1281},
    {233, {"b\351", 2}, 2, jn_cut_1282},
    {233, {"b\351  \351b", 6}, 3, jn_cut_1283},
    {233, {"b\351 b", 4}, 2, jn_cut_1284},
    {233, {"b\351 b b\351\351\351b", 10}, 5, jn_cut_1285},
    {233, {"b\351ababb\351aa", 10}, 3, jn_cut_1286},
    {233, {"b\351b aa", 6}, 2, jn_cut_1287},
    {233, {"b\351babb\351b", 8}, 3, jn_cut_1288},
    {233, {"b\351\351aa\351b\351", 8}, 5, jn_cut_1289},
    {233, {"\351  b  a  ", 9}, 2, jn_cut_1290},
    {233, {"\351 a bb\351", 7}, 3, jn_cut_1291},
    {233, {"\351 \351", 3}, 3, jn_cut_1292},
    {233, {"\351aa ba b a", 10}, 2, jn_cut_1293},
    {233, {"\351abaa\351\351ab", 9}, 4, jn_cut_1294},
    {233, {"\351abba\351\351", 7}, 4, jn_cut_1295},
    {233, {"\351ab\351\351b\351b\351", 9}, 6, jn_cut_1296},
    {233, {"\351a\351", 3}, 3, jn_cut_1297},
    {233, {"\351a\351 ", 4}, 3, jn_cut_1298},
    {233, {"\351a\351\351 ", 5}, 4, jn_cut_1299},
    {233, {"\351a\351\351 \351\351", 7}, 6, jn_cut_1300},
    {233, {"\351b", 2}, 2, jn_cut_1301},
    {233, {"\351baa\351 \351 \351b", 10}, 5, jn_cut_1302},
    {233, {"\351bab  ", 6}, 2, jn_cut_1303},
    {233, {"\351ba\351\351\351 ", 7}, 5, jn_cut_1304},
    {233, {"\351bba\351a ", 7}, 3, jn_cut_1305},
    {233, {"\351bbb\351bb", 7}, 3, jn_cut_1306},
    {233, {"\351b\351\351b\351\351", 7}, 6, jn_cut_1307},
    {233, {"\351\351 b\351 a\351", 8}, 5, jn_cut_1308},
    {233, {"\351\351a \351bba  ", 10}, 4, jn_cut_1309},
    {233, {"\351\351\351ba\351\351ba", 9}, 6, jn_cut_1310},
    {10, {"", 0}, 0, 0},
    {10, {"a", 1}, 1, jn_cut_1312},
    {10, {" ", 1}, 1, jn_cut_1313},
    {10, {"a b", 3}, 1, jn_cut_1314},
    {10, {"a,b", 3}, 1, jn_cut_1315},
    {10, {"\011", 1}, 1, jn_cut_1316},
    {10, {"  x  ", 5}, 1, jn_cut_1317},
    {10, {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 300}, 1, jn_cut_1318},
    {10, {"a\012b", 3}, 1, jn_cut_1319},
    {10, {"\012", 1}, 1, jn_cut_1320},
    {10, {"a\012", 2}, 1, jn_cut_1321},
    {10, {"\012a", 2}, 1, jn_cut_1322},
    {10, {"a\012\012b", 4}, 1, jn_cut_1323},
    {10, {"a b\012c d", 7}, 1, jn_cut_1324},
};

static const jn_key_case_t jn_key_cases[] = {
    {{"a", 1}, {"A", 1}, 0, 1},
    {{"a", 1}, {"A", 1}, 1, 0},
    {{"A", 1}, {"a", 1}, 0, -1},
    {{"A", 1}, {"a", 1}, 1, 0},
    {{"a", 1}, {"B", 1}, 0, 1},
    {{"a", 1}, {"B", 1}, 1, -1},
    {{"A", 1}, {"b", 1}, 0, -1},
    {{"A", 1}, {"b", 1}, 1, -1},
    {{"Z", 1}, {"a", 1}, 0, -1},
    {{"Z", 1}, {"a", 1}, 1, 1},
    {{"z", 1}, {"A", 1}, 0, 1},
    {{"z", 1}, {"A", 1}, 1, 1},
    {{"_", 1}, {"a", 1}, 0, -1},
    {{"_", 1}, {"a", 1}, 1, 1},
    {{"_", 1}, {"A", 1}, 0, 1},
    {{"_", 1}, {"A", 1}, 1, 1},
    {{"[", 1}, {"a", 1}, 0, -1},
    {{"[", 1}, {"a", 1}, 1, 1},
    {{"`", 1}, {"A", 1}, 0, 1},
    {{"`", 1}, {"A", 1}, 1, 1},
    {{"{", 1}, {"Z", 1}, 0, 1},
    {{"{", 1}, {"Z", 1}, 1, 1},
    {{"@", 1}, {"a", 1}, 0, -1},
    {{"@", 1}, {"a", 1}, 1, -1},
    {{"", 0}, {"", 0}, 0, 0},
    {{"", 0}, {"", 0}, 1, 0},
    {{"", 0}, {"a", 1}, 0, -1},
    {{"", 0}, {"a", 1}, 1, -1},
    {{"a", 1}, {"", 0}, 0, 1},
    {{"a", 1}, {"", 0}, 1, 1},
    {{"ab", 2}, {"a", 1}, 0, 1},
    {{"ab", 2}, {"a", 1}, 1, 1},
    {{"a", 1}, {"ab", 2}, 0, -1},
    {{"a", 1}, {"ab", 2}, 1, -1},
    {{"ab", 2}, {"AB", 2}, 0, 1},
    {{"ab", 2}, {"AB", 2}, 1, 0},
    {{"aB", 2}, {"Ab", 2}, 0, 1},
    {{"aB", 2}, {"Ab", 2}, 1, 0},
    {{"abc", 3}, {"ABD", 3}, 0, 1},
    {{"abc", 3}, {"ABD", 3}, 1, -1},
    {{"abc", 3}, {"abd", 3}, 0, -1},
    {{"abc", 3}, {"abd", 3}, 1, -1},
    {{"\351", 1}, {"\311", 1}, 0, 1},
    {{"\351", 1}, {"\311", 1}, 1, 1},
    {{"\377", 1}, {"a", 1}, 0, 1},
    {{"\377", 1}, {"a", 1}, 1, 1},
    {{"a", 1}, {"\200", 1}, 0, -1},
    {{"a", 1}, {"\200", 1}, 1, -1},
    {{"a b", 3}, {"a", 1}, 0, 1},
    {{"a b", 3}, {"a", 1}, 1, 1},
    {{"a\011b", 3}, {"a b", 3}, 0, -1},
    {{"a\011b", 3}, {"a b", 3}, 1, -1},
    {{"\000", 1}, {"", 0}, 0, 1},
    {{"\000", 1}, {"", 0}, 1, 1},
    {{"\000", 1}, {"\000", 1}, 0, 0},
    {{"\000", 1}, {"\000", 1}, 1, 0},
    {{"a\000b", 3}, {"a\000c", 3}, 0, -1},
    {{"a\000b", 3}, {"a\000c", 3}, 1, -1},
    {{"a\000", 2}, {"a", 1}, 0, 1},
    {{"a\000", 2}, {"a", 1}, 1, 1},
    {{"Hello", 5}, {"hello", 5}, 0, -1},
    {{"Hello", 5}, {"hello", 5}, 1, 0},
    {{"hello", 5}, {"help", 4}, 0, -1},
    {{"hello", 5}, {"help", 4}, 1, -1},
    {{"x", 1}, {"x", 1}, 0, 0},
    {{"x", 1}, {"x", 1}, 1, 0},
    {{"X", 1}, {"x", 1}, 0, -1},
    {{"X", 1}, {"x", 1}, 1, 0},
    {{" \303\303", 3}, {"\011_Z\377", 4}, 0, 1},
    {{" \303\303", 3}, {"\011_Z\377", 4}, 1, 1},
    {{"ZB\303", 3}, {"zb", 2}, 0, -1},
    {{"ZB\303", 3}, {"zb", 2}, 1, 1},
    {{"A\177\303Z", 4}, {"a\377bA", 4}, 0, -1},
    {{"A\177\303Z", 4}, {"a\377bA", 4}, 1, -1},
    {{"", 0}, {"[", 1}, 0, -1},
    {{"", 0}, {"[", 1}, 1, -1},
    {{"a\303@\303", 4}, {"_\377[{", 4}, 0, 1},
    {{"a\303@\303", 4}, {"_\377[{", 4}, 1, -1},
    {{"ab\303", 3}, {"\200 ", 2}, 0, -1},
    {{"ab\303", 3}, {"\200 ", 2}, 1, -1},
    {{"", 0}, {"@[", 2}, 0, -1},
    {{"", 0}, {"@[", 2}, 1, -1},
    {{"{ab\011", 4}, {"", 0}, 0, 1},
    {{"{ab\011", 4}, {"", 0}, 1, 1},
    {{"B{\177", 3}, {"", 0}, 0, 1},
    {{"B{\177", 3}, {"", 0}, 1, 1},
    {{"", 0}, {"", 0}, 0, 0},
    {{"", 0}, {"", 0}, 1, 0},
    {{"_", 1}, {"", 0}, 0, 1},
    {{"_", 1}, {"", 0}, 1, 1},
    {{"\177\177\200", 3}, {"", 0}, 0, 1},
    {{"\177\177\200", 3}, {"", 0}, 1, 1},
    {{"_`@b", 4}, {"@a", 2}, 0, 1},
    {{"_`@b", 4}, {"@a", 2}, 1, 1},
    {{"Bz[", 3}, {"", 0}, 0, 1},
    {{"Bz[", 3}, {"", 0}, 1, 1},
    {{"", 0}, {"", 0}, 0, 0},
    {{"", 0}, {"", 0}, 1, 0},
    {{"\343Z ", 3}, {"\303", 1}, 0, 1},
    {{"\343Z ", 3}, {"\303", 1}, 1, 1},
    {{"_z\200\177", 4}, {"", 0}, 0, 1},
    {{"_z\200\177", 4}, {"", 0}, 1, 1},
    {{"\200_a", 3}, {"\011{", 2}, 0, 1},
    {{"\200_a", 3}, {"\011{", 2}, 1, 1},
    {{"", 0}, {"Z", 1}, 0, -1},
    {{"", 0}, {"Z", 1}, 1, -1},
    {{"\011BA", 3}, {"_", 1}, 0, -1},
    {{"\011BA", 3}, {"_", 1}, 1, -1},
    {{"`a@", 3}, {"\177b", 2}, 0, -1},
    {{"`a@", 3}, {"\177b", 2}, 1, -1},
    {{"", 0}, {"", 0}, 0, 0},
    {{"", 0}, {"", 0}, 1, 0},
    {{"\011", 1}, {"a", 1}, 0, -1},
    {{"\011", 1}, {"a", 1}, 1, -1},
    {{"00\303z", 4}, {"\343\011z\177", 4}, 0, -1},
    {{"00\303z", 4}, {"\343\011z\177", 4}, 1, -1},
    {{"z", 1}, {"[[", 2}, 0, 1},
    {{"z", 1}, {"[[", 2}, 1, -1},
    {{"Z", 1}, {"_\177\343b", 4}, 0, -1},
    {{"Z", 1}, {"_\177\343b", 4}, 1, -1},
    {{"ABB", 3}, {"", 0}, 0, 1},
    {{"ABB", 3}, {"", 0}, 1, 1},
    {{"`[\177`", 4}, {"\343{\377", 3}, 0, -1},
    {{"`[\177`", 4}, {"\343{\377", 3}, 1, -1},
    {{"b", 1}, {"[", 1}, 0, 1},
    {{"b", 1}, {"[", 1}, 1, -1},
    {{" b`", 3}, {"_", 1}, 0, -1},
    {{" b`", 3}, {"_", 1}, 1, -1},
    {{"", 0}, {"", 0}, 0, 0},
    {{"", 0}, {"", 0}, 1, 0},
    {{"\200\303", 2}, {"A", 1}, 0, 1},
    {{"\200\303", 2}, {"A", 1}, 1, 1},
    {{"", 0}, {"{", 1}, 0, -1},
    {{"", 0}, {"{", 1}, 1, -1},
    {{"\377\011", 2}, {"b", 1}, 0, 1},
    {{"\377\011", 2}, {"b", 1}, 1, 1},
    {{"z\303", 2}, {"\377\011", 2}, 0, -1},
    {{"z\303", 2}, {"\377\011", 2}, 1, -1},
    {{"\011", 1}, {"", 0}, 0, 1},
    {{"\011", 1}, {"", 0}, 1, 1},
    {{"", 0}, {"0{A", 3}, 0, -1},
    {{"", 0}, {"0{A", 3}, 1, -1},
    {{"", 0}, {"b\343b{", 4}, 0, -1},
    {{"", 0}, {"b\343b{", 4}, 1, -1},
    {{"zb", 2}, {"", 0}, 0, 1},
    {{"zb", 2}, {"", 0}, 1, 1},
    {{" 0A", 3}, {"@", 1}, 0, -1},
    {{" 0A", 3}, {"@", 1}, 1, -1},
    {{"b\343", 2}, {"", 0}, 0, 1},
    {{"b\343", 2}, {"", 0}, 1, 1},
    {{"a\343\011", 3}, {"", 0}, 0, 1},
    {{"a\343\011", 3}, {"", 0}, 1, 1},
    {{"\177\177\011a", 4}, {"bbbB", 4}, 0, 1},
    {{"\177\177\011a", 4}, {"bbbB", 4}, 1, 1},
    {{"\200@", 2}, {"\011\303\303", 3}, 0, 1},
    {{"\200@", 2}, {"\011\303\303", 3}, 1, 1},
    {{" b\377", 3}, {"a{b\343", 4}, 0, -1},
    {{" b\377", 3}, {"a{b\343", 4}, 1, -1},
    {{"", 0}, {"B", 1}, 0, -1},
    {{"", 0}, {"B", 1}, 1, -1},
    {{"\343`a", 3}, {"{z", 2}, 0, 1},
    {{"\343`a", 3}, {"{z", 2}, 1, 1},
    {{"_\377Z@", 4}, {"\343[@", 3}, 0, -1},
    {{"_\377Z@", 4}, {"\343[@", 3}, 1, -1},
    {{"`_\200", 3}, {"_", 1}, 0, 1},
    {{"`_\200", 3}, {"_", 1}, 1, 1},
    {{"[\011@", 3}, {"z", 1}, 0, -1},
    {{"[\011@", 3}, {"z", 1}, 1, 1},
    {{"\343", 1}, {"Ab", 2}, 0, 1},
    {{"\343", 1}, {"Ab", 2}, 1, 1},
    {{"ZB", 2}, {"\343`_", 3}, 0, -1},
    {{"ZB", 2}, {"\343`_", 3}, 1, -1},
    {{"\177\377\343", 3}, {"\303@", 2}, 0, -1},
    {{"\177\377\343", 3}, {"\303@", 2}, 1, -1},
    {{"", 0}, {"", 0}, 0, 0},
    {{"", 0}, {"", 0}, 1, 0},
    {{"A`", 2}, {"0{\011a", 4}, 0, 1},
    {{"A`", 2}, {"0{\011a", 4}, 1, 1},
    {{"\177", 1}, {"_a`", 3}, 0, 1},
    {{"\177", 1}, {"_a`", 3}, 1, 1},
    {{"z", 1}, {"", 0}, 0, 1},
    {{"z", 1}, {"", 0}, 1, 1},
    {{"", 0}, {"B 0", 3}, 0, -1},
    {{"", 0}, {"B 0", 3}, 1, -1},
    {{"", 0}, {"_", 1}, 0, -1},
    {{"", 0}, {"_", 1}, 1, -1},
    {{"`Za", 3}, {" AZ", 3}, 0, 1},
    {{"`Za", 3}, {" AZ", 3}, 1, 1},
    {{"`", 1}, {" \377", 2}, 0, 1},
    {{"`", 1}, {" \377", 2}, 1, 1},
    {{"Z\177[b", 4}, {"\177z\303", 3}, 0, -1},
    {{"Z\177[b", 4}, {"\177z\303", 3}, 1, -1},
    {{"_a\177", 3}, {"\011\377@\303", 4}, 0, 1},
    {{"_a\177", 3}, {"\011\377@\303", 4}, 1, 1},
    {{"_B", 2}, {"", 0}, 0, 1},
    {{"_B", 2}, {"", 0}, 1, 1},
    {{"[", 1}, {"b{ ", 3}, 0, -1},
    {{"[", 1}, {"b{ ", 3}, 1, 1},
    {{"`a", 2}, {"\377b", 2}, 0, -1},
    {{"`a", 2}, {"\377b", 2}, 1, -1},
    {{"", 0}, {"@ \200", 3}, 0, -1},
    {{"", 0}, {"@ \200", 3}, 1, -1},
    {{"\343a", 2}, {"b", 1}, 0, 1},
    {{"\343a", 2}, {"b", 1}, 1, 1},
    {{"AZ ", 3}, {"z\343", 2}, 0, -1},
    {{"AZ ", 3}, {"z\343", 2}, 1, -1},
    {{"\377", 1}, {"\303\343\011b", 4}, 0, 1},
    {{"\377", 1}, {"\303\343\011b", 4}, 1, 1},
    {{"\303", 1}, {" { Z", 4}, 0, 1},
    {{"\303", 1}, {" { Z", 4}, 1, 1},
    {{"\377 `{", 4}, {"_{z", 3}, 0, 1},
    {{"\377 `{", 4}, {"_{z", 3}, 1, 1},
    {{"\377`\011\343", 4}, {"\200", 1}, 0, 1},
    {{"\377`\011\343", 4}, {"\200", 1}, 1, 1},
    {{"B\377a\177", 4}, {"", 0}, 0, 1},
    {{"B\377a\177", 4}, {"", 0}, 1, 1},
    {{"A\377\177 ", 4}, {"B\343bZ", 4}, 0, -1},
    {{"A\377\177 ", 4}, {"B\343bZ", 4}, 1, -1},
    {{"", 0}, {"\303\200\177`", 4}, 0, -1},
    {{"", 0}, {"\303\200\177`", 4}, 1, -1},
    {{"\343", 1}, {"z@\200", 3}, 0, 1},
    {{"\343", 1}, {"z@\200", 3}, 1, 1},
    {{"\377@B", 3}, {"\200", 1}, 0, 1},
    {{"\377@B", 3}, {"\200", 1}, 1, 1},
    {{"a`za", 4}, {"", 0}, 0, 1},
    {{"a`za", 4}, {"", 0}, 1, 1},
    {{"z", 1}, {"a", 1}, 0, 1},
    {{"z", 1}, {"a", 1}, 1, 1},
    {{"@0", 2}, {"\343", 1}, 0, -1},
    {{"@0", 2}, {"\343", 1}, 1, -1},
    {{"", 0}, {"\011B\377", 3}, 0, -1},
    {{"", 0}, {"\011B\377", 3}, 1, -1},
    {{"`_\377\200", 4}, {"", 0}, 0, 1},
    {{"`_\377\200", 4}, {"", 0}, 1, 1},
    {{"\200\377Z", 3}, {"_ _\377", 4}, 0, 1},
    {{"\200\377Z", 3}, {"_ _\377", 4}, 1, 1},
    {{" ", 1}, {"\011z[0", 4}, 0, 1},
    {{" ", 1}, {"\011z[0", 4}, 1, 1},
    {{"@", 1}, {"@___", 4}, 0, -1},
    {{"@", 1}, {"@___", 4}, 1, -1},
    {{"", 0}, {"[", 1}, 0, -1},
    {{"", 0}, {"[", 1}, 1, -1},
    {{"b", 1}, {"\177B", 2}, 0, -1},
    {{"b", 1}, {"\177B", 2}, 1, -1},
    {{"\200 z", 3}, {"\177", 1}, 0, 1},
    {{"\200 z", 3}, {"\177", 1}, 1, 1},
    {{"", 0}, {"", 0}, 0, 0},
    {{"", 0}, {"", 0}, 1, 0},
    {{"\011", 1}, {"0B", 2}, 0, -1},
    {{"\011", 1}, {"0B", 2}, 1, -1},
    {{"@\377_b", 4}, {"BaA", 3}, 0, -1},
    {{"@\377_b", 4}, {"BaA", 3}, 1, -1},
    {{"\377\011\343z", 4}, {"Z", 1}, 0, 1},
    {{"\377\011\343z", 4}, {"Z", 1}, 1, 1},
    {{"", 0}, {"Z", 1}, 0, -1},
    {{"", 0}, {"Z", 1}, 1, -1},
    {{"{", 1}, {"", 0}, 0, 1},
    {{"{", 1}, {"", 0}, 1, 1},
    {{"Az\303b", 4}, {"", 0}, 0, 1},
    {{"Az\303b", 4}, {"", 0}, 1, 1},
    {{"\177\303", 2}, {"\3770\200", 3}, 0, -1},
    {{"\177\303", 2}, {"\3770\200", 3}, 1, -1},
    {{"0", 1}, {"", 0}, 0, 1},
    {{"0", 1}, {"", 0}, 1, 1},
    {{"", 0}, {"Z", 1}, 0, -1},
    {{"", 0}, {"Z", 1}, 1, -1},
    {{"\30300", 3}, {"_ZB", 3}, 0, 1},
    {{"\30300", 3}, {"_ZB", 3}, 1, 1},
    {{"a@b\177", 4}, {"_\377\011@", 4}, 0, 1},
    {{"a@b\177", 4}, {"_\377\011@", 4}, 1, -1},
    {{"`B", 2}, {"\177", 1}, 0, -1},
    {{"`B", 2}, {"\177", 1}, 1, -1},
    {{"@", 1}, {"0\200Z\177", 4}, 0, 1},
    {{"@", 1}, {"0\200Z\177", 4}, 1, 1},
    {{"Z", 1}, {"", 0}, 0, 1},
    {{"Z", 1}, {"", 0}, 1, 1},
    {{"{\343", 2}, {"", 0}, 0, 1},
    {{"{\343", 2}, {"", 0}, 1, 1},
    {{"", 0}, {"A[", 2}, 0, -1},
    {{"", 0}, {"A[", 2}, 1, -1},
    {{"{@", 2}, {"\177", 1}, 0, -1},
    {{"{@", 2}, {"\177", 1}, 1, -1},
    {{"Z b\177", 4}, {"\343_B\177", 4}, 0, -1},
    {{"Z b\177", 4}, {"\343_B\177", 4}, 1, -1},
    {{"aBB[", 4}, {"\303\177", 2}, 0, -1},
    {{"aBB[", 4}, {"\303\177", 2}, 1, -1},
    {{"A_\177a", 4}, {"", 0}, 0, 1},
    {{"A_\177a", 4}, {"", 0}, 1, 1},
    {{"``", 2}, {"  ", 2}, 0, 1},
    {{"``", 2}, {"  ", 2}, 1, 1},
    {{"\200\377\011B", 4}, {"b A", 3}, 0, 1},
    {{"\200\377\011B", 4}, {"b A", 3}, 1, 1},
    {{"Z\177\343", 3}, {"\343", 1}, 0, -1},
    {{"Z\177\343", 3}, {"\343", 1}, 1, -1},
    {{"A\200\343\200", 4}, {"\377\177", 2}, 0, -1},
    {{"A\200\343\200", 4}, {"\377\177", 2}, 1, -1},
    {{"{0\377{", 4}, {"` {", 3}, 0, 1},
    {{"{0\377{", 4}, {"` {", 3}, 1, 1},
    {{"aa", 2}, {"\011", 1}, 0, 1},
    {{"aa", 2}, {"\011", 1}, 1, 1},
    {{"", 0}, {"\200", 1}, 0, -1},
    {{"", 0}, {"\200", 1}, 1, -1},
    {{"A@\177", 3}, {"", 0}, 0, 1},
    {{"A@\177", 3}, {"", 0}, 1, 1},
    {{"@b[\200", 4}, {"`[A", 3}, 0, -1},
    {{"@b[\200", 4}, {"`[A", 3}, 1, -1},
    {{"B\303z[", 4}, {"BA\200\303", 4}, 0, 1},
    {{"B\303z[", 4}, {"BA\200\303", 4}, 1, 1},
    {{"", 0}, {"A", 1}, 0, -1},
    {{"", 0}, {"A", 1}, 1, -1},
    {{"\377z", 2}, {"", 0}, 0, 1},
    {{"\377z", 2}, {"", 0}, 1, 1},
    {{"\303z", 2}, {"\303`\011", 3}, 0, 1},
    {{"\303z", 2}, {"\303`\011", 3}, 1, -1},
    {{"0\377z", 3}, {"z[", 2}, 0, -1},
    {{"0\377z", 3}, {"z[", 2}, 1, -1},
    {{"B\377{", 3}, {"0``\011", 4}, 0, 1},
    {{"B\377{", 3}, {"0``\011", 4}, 1, 1},
    {{"_`[_", 4}, {"\377", 1}, 0, -1},
    {{"_`[_", 4}, {"\377", 1}, 1, -1},
    {{"A", 1}, {"", 0}, 0, 1},
    {{"A", 1}, {"", 0}, 1, 1},
    {{"", 0}, {"`\200", 2}, 0, -1},
    {{"", 0}, {"`\200", 2}, 1, -1},
    {{"", 0}, {"AB[ ", 4}, 0, -1},
    {{"", 0}, {"AB[ ", 4}, 1, -1},
    {{"bb", 2}, {" ", 1}, 0, 1},
    {{"bb", 2}, {" ", 1}, 1, 1},
    {{"0", 1}, {"\3430_", 3}, 0, -1},
    {{"0", 1}, {"\3430_", 3}, 1, -1},
    {{"@\343", 2}, {"b", 1}, 0, -1},
    {{"@\343", 2}, {"b", 1}, 1, -1},
    {{"", 0}, {"_\303\200", 3}, 0, -1},
    {{"", 0}, {"_\303\200", 3}, 1, -1},
    {{"\177z", 2}, {"z@", 2}, 0, 1},
    {{"\177z", 2}, {"z@", 2}, 1, 1},
    {{" Z", 2}, {"0\011B", 3}, 0, -1},
    {{" Z", 2}, {"0\011B", 3}, 1, -1},
    {{"@b ", 3}, {"", 0}, 0, 1},
    {{"@b ", 3}, {"", 0}, 1, 1},
    {{"\011\011\343", 3}, {"{ab", 3}, 0, -1},
    {{"\011\011\343", 3}, {"{ab", 3}, 1, -1},
    {{"_b", 2}, {"\343@", 2}, 0, -1},
    {{"_b", 2}, {"\343@", 2}, 1, -1},
    {{"z[", 2}, {"@0", 2}, 0, 1},
    {{"z[", 2}, {"@0", 2}, 1, 1},
    {{"", 0}, {"\303\011", 2}, 0, -1},
    {{"", 0}, {"\303\011", 2}, 1, -1},
    {{"`\303\377{", 4}, {"@[\177", 3}, 0, 1},
    {{"`\303\377{", 4}, {"@[\177", 3}, 1, 1},
    {{"[b00", 4}, {"", 0}, 0, 1},
    {{"[b00", 4}, {"", 0}, 1, 1},
    {{"\177\011", 2}, {"_\0110", 3}, 0, 1},
    {{"\177\011", 2}, {"_\0110", 3}, 1, 1},
    {{" z\011", 3}, {"ZZb\303", 4}, 0, -1},
    {{" z\011", 3}, {"ZZb\303", 4}, 1, -1},
    {{"a[", 2}, {"A Z\011", 4}, 0, 1},
    {{"a[", 2}, {"A Z\011", 4}, 1, 1},
    {{"a\200", 2}, {"", 0}, 0, 1},
    {{"a\200", 2}, {"", 0}, 1, 1},
    {{" { b", 4}, {"b`", 2}, 0, -1},
    {{" { b", 4}, {"b`", 2}, 1, -1},
    {{"", 0}, {"ba", 2}, 0, -1},
    {{"", 0}, {"ba", 2}, 1, -1},
    {{"B", 1}, {"[[\343", 3}, 0, -1},
    {{"B", 1}, {"[[\343", 3}, 1, -1},
    {{"@\303\1770", 4}, {"@z", 2}, 0, 1},
    {{"@\303\1770", 4}, {"@z", 2}, 1, 1},
    {{"\343 b", 3}, {"A\2000a", 4}, 0, 1},
    {{"\343 b", 3}, {"A\2000a", 4}, 1, 1},
    {{"b\303 ", 3}, {"", 0}, 0, 1},
    {{"b\303 ", 3}, {"", 0}, 1, 1},
    {{"0aB\200", 4}, {"z[Z", 3}, 0, -1},
    {{"0aB\200", 4}, {"z[Z", 3}, 1, -1},
    {{"Z@_", 3}, {"\200\377{", 3}, 0, -1},
    {{"Z@_", 3}, {"\200\377{", 3}, 1, -1},
    {{"A\343", 2}, {"B{", 2}, 0, -1},
    {{"A\343", 2}, {"B{", 2}, 1, -1},
    {{"Z", 1}, {"", 0}, 0, 1},
    {{"Z", 1}, {"", 0}, 1, 1},
    {{"a\011\343", 3}, {"", 0}, 0, 1},
    {{"a\011\343", 3}, {"", 0}, 1, 1},
    {{"B_", 2}, {"0", 1}, 0, 1},
    {{"B_", 2}, {"0", 1}, 1, 1},
    {{"\011", 1}, {"\343", 1}, 0, -1},
    {{"\011", 1}, {"\343", 1}, 1, -1},
    {{"\377[\303Z", 4}, {"\177Z", 2}, 0, 1},
    {{"\377[\303Z", 4}, {"\177Z", 2}, 1, 1},
    {{"  ", 2}, {"`\343\177", 3}, 0, -1},
    {{"  ", 2}, {"`\343\177", 3}, 1, -1},
    {{" b", 2}, {"[\177A", 3}, 0, -1},
    {{" b", 2}, {"[\177A", 3}, 1, -1},
    {{"z", 1}, {"\377\377`", 3}, 0, -1},
    {{"z", 1}, {"\377\377`", 3}, 1, -1},
    {{"", 0}, {"a", 1}, 0, -1},
    {{"", 0}, {"a", 1}, 1, -1},
    {{"0\303[", 3}, {"Z\200Z", 3}, 0, -1},
    {{"0\303[", 3}, {"Z\200Z", 3}, 1, -1},
    {{"z`", 2}, {"z`\377 ", 4}, 0, -1},
    {{"z`", 2}, {"z`\377 ", 4}, 1, -1},
    {{"", 0}, {"\011", 1}, 0, -1},
    {{"", 0}, {"\011", 1}, 1, -1},
    {{"a", 1}, {"z", 1}, 0, -1},
    {{"a", 1}, {"z", 1}, 1, -1},
    {{"b", 1}, {"`\377", 2}, 0, 1},
    {{"b", 1}, {"`\377", 2}, 1, -1},
    {{"", 0}, {"\303b\011", 3}, 0, -1},
    {{"", 0}, {"\303b\011", 3}, 1, -1},
    {{"\377{a_", 4}, {"_b\303", 3}, 0, 1},
    {{"\377{a_", 4}, {"_b\303", 3}, 1, 1},
    {{"A", 1}, {"\200\377\343Z", 4}, 0, -1},
    {{"A", 1}, {"\200\377\343Z", 4}, 1, -1},
    {{"@{", 2}, {"b {", 3}, 0, -1},
    {{"@{", 2}, {"b {", 3}, 1, -1},
    {{"b0b", 3}, {"B", 1}, 0, 1},
    {{"b0b", 3}, {"B", 1}, 1, 1},
    {{"\303B\303", 3}, {"", 0}, 0, 1},
    {{"\303B\303", 3}, {"", 0}, 1, 1},
    {{"\343[{", 3}, {"B\303a_", 4}, 0, 1},
    {{"\343[{", 3}, {"B\303a_", 4}, 1, 1},
    {{"z{\177 ", 4}, {"@\177", 2}, 0, 1},
    {{"z{\177 ", 4}, {"@\177", 2}, 1, 1},
    {{"b`__", 4}, {"B@{", 3}, 0, 1},
    {{"b`__", 4}, {"B@{", 3}, 1, 1},
    {{"\011\343", 2}, {"\303\177", 2}, 0, -1},
    {{"\011\343", 2}, {"\303\177", 2}, 1, -1},
    {{"", 0}, {"", 0}, 0, 0},
    {{"", 0}, {"", 0}, 1, 0},
    {{"B", 1}, {"Z", 1}, 0, -1},
    {{"B", 1}, {"Z", 1}, 1, -1},
    {{"_", 1}, {"A\303\011", 3}, 0, 1},
    {{"_", 1}, {"A\303\011", 3}, 1, 1},
    {{"", 0}, {"", 0}, 0, 0},
    {{"", 0}, {"", 0}, 1, 0},
    {{"A", 1}, {"B", 1}, 0, -1},
    {{"A", 1}, {"B", 1}, 1, -1},
    {{"\200b@", 3}, {"\377z_", 3}, 0, -1},
    {{"\200b@", 3}, {"\377z_", 3}, 1, -1},
    {{"\177", 1}, {"@\200a", 3}, 0, 1},
    {{"\177", 1}, {"@\200a", 3}, 1, 1},
    {{"@[\377", 3}, {"", 0}, 0, 1},
    {{"@[\377", 3}, {"", 0}, 1, 1},
    {{"@_", 2}, {"{\303", 2}, 0, -1},
    {{"@_", 2}, {"{\303", 2}, 1, -1},
    {{"", 0}, {"\177_\377@", 4}, 0, -1},
    {{"", 0}, {"\177_\377@", 4}, 1, -1},
    {{"BZ", 2}, {" {", 2}, 0, 1},
    {{"BZ", 2}, {" {", 2}, 1, 1},
    {{"[0\377\377", 4}, {"\011", 1}, 0, 1},
    {{"[0\377\377", 4}, {"\011", 1}, 1, 1},
    {{"\377", 1}, {"\011", 1}, 0, 1},
    {{"\377", 1}, {"\011", 1}, 1, 1},
    {{"", 0}, {"b", 1}, 0, -1},
    {{"", 0}, {"b", 1}, 1, -1},
    {{"[", 1}, {"\011\200@", 3}, 0, 1},
    {{"[", 1}, {"\011\200@", 3}, 1, 1},
    {{"b0", 2}, {"\303B", 2}, 0, -1},
    {{"b0", 2}, {"\303B", 2}, 1, -1},
    {{"z[\011", 3}, {"  b ", 4}, 0, 1},
    {{"z[\011", 3}, {"  b ", 4}, 1, 1},
    {{"Z@", 2}, {"zBa{", 4}, 0, -1},
    {{"Z@", 2}, {"zBa{", 4}, 1, -1},
    {{"[\011", 2}, {"ZZ", 2}, 0, 1},
    {{"[\011", 2}, {"ZZ", 2}, 1, 1},
    {{" b\303A", 4}, {"Zz{", 3}, 0, -1},
    {{" b\303A", 4}, {"Zz{", 3}, 1, -1},
    {{"\011aZ", 3}, {"\200", 1}, 0, -1},
    {{"\011aZ", 3}, {"\200", 1}, 1, -1},
    {{"Az\377", 3}, {"B\011\177", 3}, 0, -1},
    {{"Az\377", 3}, {"B\011\177", 3}, 1, -1},
    {{"@", 1}, {"z`[\011", 4}, 0, -1},
    {{"@", 1}, {"z`[\011", 4}, 1, -1},
    {{"AA", 2}, {"\177", 1}, 0, -1},
    {{"AA", 2}, {"\177", 1}, 1, -1},
    {{"B\3430", 3}, {"\011\200{", 3}, 0, 1},
    {{"B\3430", 3}, {"\011\200{", 3}, 1, 1},
    {{"\303\200\200", 3}, {"", 0}, 0, 1},
    {{"\303\200\200", 3}, {"", 0}, 1, 1},
    {{"B\377@\377", 4}, {"\200\200a@", 4}, 0, -1},
    {{"B\377@\377", 4}, {"\200\200a@", 4}, 1, -1},
    {{"", 0}, {"\200\177", 2}, 0, -1},
    {{"", 0}, {"\200\177", 2}, 1, -1},
    {{"@{\011b", 4}, {"B", 1}, 0, -1},
    {{"@{\011b", 4}, {"B", 1}, 1, -1},
    {{"[\377\177", 3}, {"", 0}, 0, 1},
    {{"[\377\177", 3}, {"", 0}, 1, 1},
    {{"B\377[", 3}, {"{\177[b", 4}, 0, -1},
    {{"B\377[", 3}, {"{\177[b", 4}, 1, -1},
    {{" \200", 2}, {"Z[\200\011", 4}, 0, -1},
    {{" \200", 2}, {"Z[\200\011", 4}, 1, -1},
    {{"", 0}, {"", 0}, 0, 0},
    {{"", 0}, {"", 0}, 1, 0},
    {{"", 0}, {"zB \011", 4}, 0, -1},
    {{"", 0}, {"zB \011", 4}, 1, -1},
    {{"a{[", 3}, {"a@", 2}, 0, 1},
    {{"a{[", 3}, {"a@", 2}, 1, 1},
    {{"b", 1}, {"\303\377\177", 3}, 0, -1},
    {{"b", 1}, {"\303\377\177", 3}, 1, -1},
    {{"0", 1}, {"[A{a", 4}, 0, -1},
    {{"0", 1}, {"[A{a", 4}, 1, -1},
    {{"@{@", 3}, {"z\343_", 3}, 0, -1},
    {{"@{@", 3}, {"z\343_", 3}, 1, -1},
    {{"{", 1}, {"", 0}, 0, 1},
    {{"{", 1}, {"", 0}, 1, 1},
    {{"@", 1}, {"\343a", 2}, 0, -1},
    {{"@", 1}, {"\343a", 2}, 1, -1},
    {{" ", 1}, {" ", 1}, 0, 0},
    {{" ", 1}, {" ", 1}, 1, 0},
    {{"_", 1}, {"", 0}, 0, 1},
    {{"_", 1}, {"", 0}, 1, 1},
    {{"", 0}, {"@`", 2}, 0, -1},
    {{"", 0}, {"@`", 2}, 1, -1},
    {{"z@\303", 3}, {"bZ[", 3}, 0, 1},
    {{"z@\303", 3}, {"bZ[", 3}, 1, 1},
    {{"B_", 2}, {"Z[", 2}, 0, -1},
    {{"B_", 2}, {"Z[", 2}, 1, -1},
    {{"{`0\377", 4}, {"", 0}, 0, 1},
    {{"{`0\377", 4}, {"", 0}, 1, 1},
    {{"\377[{Z", 4}, {"`", 1}, 0, 1},
    {{"\377[{Z", 4}, {"`", 1}, 1, 1},
    {{"b", 1}, {"b\303\377", 3}, 0, -1},
    {{"b", 1}, {"b\303\377", 3}, 1, -1},
    {{"", 0}, {"Aa\011{", 4}, 0, -1},
    {{"", 0}, {"Aa\011{", 4}, 1, -1},
    {{"`\200\011b", 4}, {"{", 1}, 0, -1},
    {{"`\200\011b", 4}, {"{", 1}, 1, -1},
    {{"[", 1}, {"`", 1}, 0, -1},
    {{"[", 1}, {"`", 1}, 1, -1},
    {{"aZzA", 4}, {"\303\011\177a", 4}, 0, -1},
    {{"aZzA", 4}, {"\303\011\177a", 4}, 1, -1},
    {{"AZb", 3}, {"0@\343A", 4}, 0, 1},
    {{"AZb", 3}, {"0@\343A", 4}, 1, 1},
    {{"{\377\377 ", 4}, {"Z", 1}, 0, 1},
    {{"{\377\377 ", 4}, {"Z", 1}, 1, 1},
    {{"", 0}, {"\200[\303 ", 4}, 0, -1},
    {{"", 0}, {"\200[\303 ", 4}, 1, -1},
    {{"[`", 2}, {" [\011", 3}, 0, 1},
    {{"[`", 2}, {" [\011", 3}, 1, 1},
    {{"\343`A_", 4}, {"A\011\303Z", 4}, 0, 1},
    {{"\343`A_", 4}, {"A\011\303Z", 4}, 1, 1},
    {{"za", 2}, {"\343\200\377\303", 4}, 0, -1},
    {{"za", 2}, {"\343\200\377\303", 4}, 1, -1},
    {{"ab", 2}, {"", 0}, 0, 1},
    {{"ab", 2}, {"", 0}, 1, 1},
    {{"\011", 1}, {"\011[zA", 4}, 0, -1},
    {{"\011", 1}, {"\011[zA", 4}, 1, -1},
    {{"\200A", 2}, {"\377`", 2}, 0, -1},
    {{"\200A", 2}, {"\377`", 2}, 1, -1},
    {{"\343b\377Z", 4}, {"`[Z", 3}, 0, 1},
    {{"\343b\377Z", 4}, {"`[Z", 3}, 1, 1},
    {{"\303A@", 3}, {"\3030_", 3}, 0, 1},
    {{"\303A@", 3}, {"\3030_", 3}, 1, 1},
    {{"", 0}, {"\343", 1}, 0, -1},
    {{"", 0}, {"\343", 1}, 1, -1},
    {{"zz\303\377", 4}, {"", 0}, 0, 1},
    {{"zz\303\377", 4}, {"", 0}, 1, 1},
    {{"", 0}, {"\200", 1}, 0, -1},
    {{"", 0}, {"\200", 1}, 1, -1},
    {{"zb\177 ", 4}, {"\343\200", 2}, 0, -1},
    {{"zb\177 ", 4}, {"\343\200", 2}, 1, -1},
    {{"", 0}, {"", 0}, 0, 0},
    {{"", 0}, {"", 0}, 1, 0},
    {{"[{`B", 4}, {"[\200\303b", 4}, 0, -1},
    {{"[{`B", 4}, {"[\200\303b", 4}, 1, -1},
    {{"0", 1}, {"@_", 2}, 0, -1},
    {{"0", 1}, {"@_", 2}, 1, -1},
    {{"", 0}, {"\177", 1}, 0, -1},
    {{"", 0}, {"\177", 1}, 1, -1},
    {{"A@\011\011", 4}, {"\303", 1}, 0, -1},
    {{"A@\011\011", 4}, {"\303", 1}, 1, -1},
    {{"_\177\177", 3}, {"", 0}, 0, 1},
    {{"_\177\177", 3}, {"", 0}, 1, 1},
    {{"\200", 1}, {"[Z", 2}, 0, 1},
    {{"\200", 1}, {"[Z", 2}, 1, 1},
    {{"Z\343_\177", 4}, {"\303", 1}, 0, -1},
    {{"Z\343_\177", 4}, {"\303", 1}, 1, -1},
    {{"\343\303", 2}, {"Z\343_", 3}, 0, 1},
    {{"\343\303", 2}, {"Z\343_", 3}, 1, 1},
    {{"\177\200bA", 4}, {"0`", 2}, 0, 1},
    {{"\177\200bA", 4}, {"0`", 2}, 1, 1},
    {{"a\377Z`", 4}, {"\177\011_`", 4}, 0, -1},
    {{"a\377Z`", 4}, {"\177\011_`", 4}, 1, -1},
    {{"", 0}, {"Az", 2}, 0, -1},
    {{"", 0}, {"Az", 2}, 1, -1},
    {{"z[", 2}, {"", 0}, 0, 1},
    {{"z[", 2}, {"", 0}, 1, 1},
    {{"", 0}, {"\177\303", 2}, 0, -1},
    {{"", 0}, {"\177\303", 2}, 1, -1},
    {{"", 0}, {"@`\377", 3}, 0, -1},
    {{"", 0}, {"@`\377", 3}, 1, -1},
    {{"", 0}, {"\177[", 2}, 0, -1},
    {{"", 0}, {"\177[", 2}, 1, -1},
    {{"A_z", 3}, {"zA_{", 4}, 0, -1},
    {{"A_z", 3}, {"zA_{", 4}, 1, -1},
    {{"\200\343 ", 3}, {"\377", 1}, 0, -1},
    {{"\200\343 ", 3}, {"\377", 1}, 1, -1},
    {{"`@\303\177", 4}, {"_", 1}, 0, 1},
    {{"`@\303\177", 4}, {"_", 1}, 1, 1},
    {{" \303", 2}, {"z\177\377", 3}, 0, -1},
    {{" \303", 2}, {"z\177\377", 3}, 1, -1},
    {{"[B", 2}, {"00", 2}, 0, 1},
    {{"[B", 2}, {"00", 2}, 1, 1},
    {{"_zAa", 4}, {"\011a{", 3}, 0, 1},
    {{"_zAa", 4}, {"\011a{", 3}, 1, 1},
    {{"", 0}, {" ", 1}, 0, -1},
    {{"", 0}, {" ", 1}, 1, -1},
    {{"B0", 2}, {"`", 1}, 0, -1},
    {{"B0", 2}, {"`", 1}, 1, -1},
    {{"", 0}, {"B\177\343", 3}, 0, -1},
    {{"", 0}, {"B\177\343", 3}, 1, -1},
    {{"", 0}, {"\177@{", 3}, 0, -1},
    {{"", 0}, {"\177@{", 3}, 1, -1},
    {{"`[Z", 3}, {"_\343Z", 3}, 0, 1},
    {{"`[Z", 3}, {"_\343Z", 3}, 1, 1},
    {{"[\377", 2}, {"", 0}, 0, 1},
    {{"[\377", 2}, {"", 0}, 1, 1},
    {{"\200z", 2}, {"\011` ", 3}, 0, 1},
    {{"\200z", 2}, {"\011` ", 3}, 1, 1},
    {{"a\177", 2}, {"", 0}, 0, 1},
    {{"a\177", 2}, {"", 0}, 1, 1},
    {{"b\177", 2}, {"A\177", 2}, 0, 1},
    {{"b\177", 2}, {"A\177", 2}, 1, 1},
    {{"\011{\343", 3}, {"`b", 2}, 0, -1},
    {{"\011{\343", 3}, {"`b", 2}, 1, -1},
    {{"", 0}, {"", 0}, 0, 0},
    {{"", 0}, {"", 0}, 1, 0},
    {{"@0", 2}, {"{`Z\343", 4}, 0, -1},
    {{"@0", 2}, {"{`Z\343", 4}, 1, -1},
    {{"@0a", 3}, {"_bA ", 4}, 0, -1},
    {{"@0a", 3}, {"_bA ", 4}, 1, -1},
    {{"\200b`", 3}, {"", 0}, 0, 1},
    {{"\200b`", 3}, {"", 0}, 1, 1},
    {{"\303\177\177", 3}, {"", 0}, 0, 1},
    {{"\303\177\177", 3}, {"", 0}, 1, 1},
    {{"\377", 1}, {"{\343", 2}, 0, 1},
    {{"\377", 1}, {"{\343", 2}, 1, 1},
    {{"\343a", 2}, {"_", 1}, 0, 1},
    {{"\343a", 2}, {"_", 1}, 1, 1},
    {{"[`", 2}, {"[", 1}, 0, 1},
    {{"[`", 2}, {"[", 1}, 1, 1},
    {{"a \177a", 4}, {" ", 1}, 0, 1},
    {{"a \177a", 4}, {" ", 1}, 1, 1},
    {{" b ", 3}, {"`0", 2}, 0, -1},
    {{" b ", 3}, {"`0", 2}, 1, -1},
    {{"", 0}, {"", 0}, 0, 0},
    {{"", 0}, {"", 0}, 1, 0},
    {{"_\177", 2}, {"_", 1}, 0, 1},
    {{"_\177", 2}, {"_", 1}, 1, 1},
    {{"Z", 1}, {"\343\011", 2}, 0, -1},
    {{"Z", 1}, {"\343\011", 2}, 1, -1},
    {{"0\200", 2}, {"0 ", 2}, 0, 1},
    {{"0\200", 2}, {"0 ", 2}, 1, 1},
    {{"[[0", 3}, {"Z\377", 2}, 0, 1},
    {{"[[0", 3}, {"Z\377", 2}, 1, 1},
    {{"b", 1}, {"", 0}, 0, 1},
    {{"b", 1}, {"", 0}, 1, 1},
    {{"", 0}, {"B\303", 2}, 0, -1},
    {{"", 0}, {"B\303", 2}, 1, -1},
    {{"\343AZ\200", 4}, {"A0`", 3}, 0, 1},
    {{"\343AZ\200", 4}, {"A0`", 3}, 1, 1},
    {{"\377\177`", 3}, {"0@", 2}, 0, 1},
    {{"\377\177`", 3}, {"0@", 2}, 1, 1},
    {{"@", 1}, {"\011", 1}, 0, 1},
    {{"@", 1}, {"\011", 1}, 1, 1},
    {{"\011", 1}, {"z`Z", 3}, 0, -1},
    {{"\011", 1}, {"z`Z", 3}, 1, -1},
    {{"_", 1}, {"{[Z", 3}, 0, -1},
    {{"_", 1}, {"{[Z", 3}, 1, -1},
    {{"", 0}, {"\011z`", 3}, 0, -1},
    {{"", 0}, {"\011z`", 3}, 1, -1},
    {{"\200`", 2}, {"\177zA", 3}, 0, 1},
    {{"\200`", 2}, {"\177zA", 3}, 1, 1},
    {{"B", 1}, {"", 0}, 0, 1},
    {{"B", 1}, {"", 0}, 1, 1},
    {{"{", 1}, {" B", 2}, 0, 1},
    {{"{", 1}, {" B", 2}, 1, 1},
    {{"@a ", 3}, {"Z0b", 3}, 0, -1},
    {{"@a ", 3}, {"Z0b", 3}, 1, -1},
    {{"\303{", 2}, {"\177", 1}, 0, 1},
    {{"\303{", 2}, {"\177", 1}, 1, 1},
    {{"\200z\011\177", 4}, {" ", 1}, 0, 1},
    {{"\200z\011\177", 4}, {" ", 1}, 1, 1},
    {{"z\303", 2}, {"B", 1}, 0, 1},
    {{"z\303", 2}, {"B", 1}, 1, 1},
    {{"`\343\200B", 4}, {"@", 1}, 0, 1},
    {{"`\343\200B", 4}, {"@", 1}, 1, 1},
    {{"", 0}, {"\011\011", 2}, 0, -1},
    {{"", 0}, {"\011\011", 2}, 1, -1},
    {{"`", 1}, {"`@[", 3}, 0, -1},
    {{"`", 1}, {"`@[", 3}, 1, -1},
    {{"", 0}, {"_0 ", 3}, 0, -1},
    {{"", 0}, {"_0 ", 3}, 1, -1},
    {{" A", 2}, {"b", 1}, 0, -1},
    {{" A", 2}, {"b", 1}, 1, -1},
    {{"", 0}, {"ba", 2}, 0, -1},
    {{"", 0}, {"ba", 2}, 1, -1},
    {{"@", 1}, {"z_\200", 3}, 0, -1},
    {{"@", 1}, {"z_\200", 3}, 1, -1},
    {{"A\2000", 3}, {"\177 A_", 4}, 0, -1},
    {{"A\2000", 3}, {"\177 A_", 4}, 1, -1},
    {{"``_{", 4}, {"\303", 1}, 0, -1},
    {{"``_{", 4}, {"\303", 1}, 1, -1},
    {{"AZaZ", 4}, {"Bb`0", 4}, 0, -1},
    {{"AZaZ", 4}, {"Bb`0", 4}, 1, -1},
    {{"", 0}, {"{b\377\303", 4}, 0, -1},
    {{"", 0}, {"{b\377\303", 4}, 1, -1},
    {{"", 0}, {"b", 1}, 0, -1},
    {{"", 0}, {"b", 1}, 1, -1},
    {{"a\377", 2}, {"[\343", 2}, 0, 1},
    {{"a\377", 2}, {"[\343", 2}, 1, -1},
    {{"{ B", 3}, {"\177\200`", 3}, 0, -1},
    {{"{ B", 3}, {"\177\200`", 3}, 1, -1},
    {{"aa\011z", 4}, {"@", 1}, 0, 1},
    {{"aa\011z", 4}, {"@", 1}, 1, 1},
    {{"0\377`", 3}, {"\3030", 2}, 0, -1},
    {{"0\377`", 3}, {"\3030", 2}, 1, -1},
    {{"`b", 2}, {"B`", 2}, 0, 1},
    {{"`b", 2}, {"B`", 2}, 1, 1},
    {{"_Z", 2}, {"A", 1}, 0, 1},
    {{"_Z", 2}, {"A", 1}, 1, 1},
    {{" ", 1}, {"_Z", 2}, 0, -1},
    {{" ", 1}, {"_Z", 2}, 1, -1},
    {{"Bz", 2}, {"0", 1}, 0, 1},
    {{"Bz", 2}, {"0", 1}, 1, 1},
    {{"Z\200", 2}, {"", 0}, 0, 1},
    {{"Z\200", 2}, {"", 0}, 1, 1},
    {{"", 0}, {"\377A\177", 3}, 0, -1},
    {{"", 0}, {"\377A\177", 3}, 1, -1},
    {{"", 0}, {" _", 2}, 0, -1},
    {{"", 0}, {" _", 2}, 1, -1},
    {{"", 0}, {"@", 1}, 0, -1},
    {{"", 0}, {"@", 1}, 1, -1},
    {{"", 0}, {"B\011@\200", 4}, 0, -1},
    {{"", 0}, {"B\011@\200", 4}, 1, -1},
    {{"[\343\200", 3}, {"`", 1}, 0, -1},
    {{"[\343\200", 3}, {"`", 1}, 1, -1},
    {{"", 0}, {"@Z0", 3}, 0, -1},
    {{"", 0}, {"@Z0", 3}, 1, -1},
    {{"az\177A", 4}, {"0", 1}, 0, 1},
    {{"az\177A", 4}, {"0", 1}, 1, 1},
    {{"\200\377\011", 3}, {"", 0}, 0, 1},
    {{"\200\377\011", 3}, {"", 0}, 1, 1},
    {{"", 0}, {"\200\303\200", 3}, 0, -1},
    {{"", 0}, {"\200\303\200", 3}, 1, -1},
    {{"\377[\177", 3}, {" \011A\377", 4}, 0, 1},
    {{"\377[\177", 3}, {" \011A\377", 4}, 1, 1},
    {{"", 0}, {"\303\303{\011", 4}, 0, -1},
    {{"", 0}, {"\303\303{\011", 4}, 1, -1},
    {{"", 0}, {"A@z ", 4}, 0, -1},
    {{"", 0}, {"A@z ", 4}, 1, -1},
    {{"@`@", 3}, {"\343", 1}, 0, -1},
    {{"@`@", 3}, {"\343", 1}, 1, -1},
    {{"\177Az\343", 4}, {"", 0}, 0, 1},
    {{"\177Az\343", 4}, {"", 0}, 1, 1},
    {{"Z", 1}, {"B", 1}, 0, 1},
    {{"Z", 1}, {"B", 1}, 1, 1},
    {{"", 0}, {"``bZ", 4}, 0, -1},
    {{"", 0}, {"``bZ", 4}, 1, -1},
    {{"", 0}, {"`\377", 2}, 0, -1},
    {{"", 0}, {"`\377", 2}, 1, -1},
    {{"\303_", 2}, {"a\200_\343", 4}, 0, 1},
    {{"\303_", 2}, {"a\200_\343", 4}, 1, 1},
    {{"[b", 2}, {"", 0}, 0, 1},
    {{"[b", 2}, {"", 0}, 1, 1},
    {{"\303\343[b", 4}, {"A b", 3}, 0, 1},
    {{"\303\343[b", 4}, {"A b", 3}, 1, 1},
    {{"0", 1}, {"B", 1}, 0, -1},
    {{"0", 1}, {"B", 1}, 1, -1},
    {{"A0", 2}, {"{", 1}, 0, -1},
    {{"A0", 2}, {"{", 1}, 1, -1},
    {{"", 0}, {"Z\177a", 3}, 0, -1},
    {{"", 0}, {"Z\177a", 3}, 1, -1},
    {{"\303\343bb", 4}, {" ", 1}, 0, 1},
    {{"\303\343bb", 4}, {" ", 1}, 1, 1},
    {{"_ ", 2}, {"\177a@", 3}, 0, -1},
    {{"_ ", 2}, {"\177a@", 3}, 1, -1},
    {{"`_", 2}, {"Z", 1}, 0, 1},
    {{"`_", 2}, {"Z", 1}, 1, 1},
    {{"Z", 1}, {"\303Az`", 4}, 0, -1},
    {{"Z", 1}, {"\303Az`", 4}, 1, -1},
    {{"", 0}, {"00", 2}, 0, -1},
    {{"", 0}, {"00", 2}, 1, -1},
    {{"_a", 2}, {"", 0}, 0, 1},
    {{"_a", 2}, {"", 0}, 1, 1},
    {{"0", 1}, {"\200_", 2}, 0, -1},
    {{"0", 1}, {"\200_", 2}, 1, -1},
    {{"b", 1}, {"\2000", 2}, 0, -1},
    {{"b", 1}, {"\2000", 2}, 1, -1},
    {{"\200`\303z", 4}, {"BB0\377", 4}, 0, 1},
    {{"\200`\303z", 4}, {"BB0\377", 4}, 1, 1},
    {{"[\343\011", 3}, {"", 0}, 0, 1},
    {{"[\343\011", 3}, {"", 0}, 1, 1},
    {{"B{", 2}, {"", 0}, 0, 1},
    {{"B{", 2}, {"", 0}, 1, 1},
    {{"[\343\177", 3}, {"\200", 1}, 0, -1},
    {{"[\343\177", 3}, {"\200", 1}, 1, -1},
    {{"[\303b0", 4}, {"@{\011", 3}, 0, 1},
    {{"[\303b0", 4}, {"@{\011", 3}, 1, 1},
    {{"`{a", 3}, {"[a\2000", 4}, 0, 1},
    {{"`{a", 3}, {"[a\2000", 4}, 1, 1},
};
