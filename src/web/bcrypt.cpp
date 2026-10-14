// =============================================================================
// 自实现 bcrypt（include/web/bcrypt.h 的配套）
// 对应任务：SPEC.md §9 M3 T3.3；规格来源 §6.2.1 / §8 R-20 / AC-49
//
// 【本文件复刻的算法骨架】（Provos & Mazières 的 EksBlowfish，见 §6.2.1 要点 1）
//   key_len   = strlen(password)，min(72) 后 +1（末尾那个 NUL 参与密钥流）
//   state     = InitState()                       // π 的十六进制小数位，见 kPiWords
//   ExpandKey(state, salt, key)                   // = Expand0(key) + Expand(salt, key)
//   repeat 2^cost: ExpandKey(state, 0, key)
//                 ExpandKey(state, 0, salt)
//   ctext     = "OrpheanBeholderScryDoubt"
//   repeat 64:  Blowfish 加密全部 3 个 64 位块
//   输出       = base64(24 字节密文) 的前 31 个字符
//
// 【为什么密钥长度用 strlen 而不是 std::string::size()】
//   参考实现（crypt_blowfish / OpenBSD / libxcrypt）全部用 strlen，于是口令里的
//   NUL 之后的内容不参与哈希。本文件照做：与所有其他实现互操作是这里的硬要求，
//   "更合理"的做法（用 size()）恰恰会让本项目算出的哈希与任何外部工具都不同。
//   代价是 "ab\0cd" 与 "ab" 是同一个口令 —— 这是 bcrypt 的既有语义，不是本文件的取舍。
//
// 【为什么 salt 在最外层、不在密钥流长度里参与循环】
//   ExpandKey(state, 0, key) 里的 "0" 是"无 salt"（Expand0State），不是"长度为 0 的 salt"。
//   两种调用在这里恰好都能用 Expand0State 表达，故实现里只有两个原语：
//   expand0state（只 XOR key）与 expandstate（key + salt 双输入）。
//
// 【栈上只有一个 BF_KEY（约 4KB），无堆分配】
//   2^cost 的循环里不分配任何内存（§0.0：1967MB 机器上与 mysqld 抢内存）。
//   cost=10 时约 210 万次 16 轮 encipher，实测 60~100ms（§6.2.1 要点 3）。
// =============================================================================
#include "web/bcrypt.h"

// OpenSSL 3.0 把 BF_encrypt 标为 deprecated（保留是为了兼容既有代码）。本项目要的正是
// 这一层"低层原语"（EVP 不暴露 P/S 状态，无法做 EksBlowfishSetup），因此显式抑制告警 ——
// 不抑制的话每次构建都会有几百行 deprecation 噪声，真正的告警反而看不见了。
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/blowfish.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace oj {
namespace web {
namespace {

static_assert(sizeof(BF_LONG) == 4, "BF_KEY 的字长必须是 32 位（Blowfish 规范）");
static_assert(BF_ROUNDS == 16, "Blowfish 固定 16 轮");

constexpr int kPLength = BF_ROUNDS + 2;   // 18
constexpr int kSLength = 4 * 256;         // 1024
// 魔文 24 字节，但只有前 23 字节参与 base64（参考实现 encode_base64(ct, 23)）
constexpr std::size_t kBcryptDigestBytes = 23;

// ---- Blowfish 初始状态（π 的十六进制小数位）--------------------------------------
// 前 18 个字是 P 数组，接下来 1024 个字依次是 S[0..3]。这是 Blowfish 算法规范的一部分
// （不是密钥、不是秘密），任何实现都带着同一张表。抄错的症状与密钥相关错误完全无法区分，
// 故 tests/bcrypt_test.cpp 用参考向量覆盖它。
constexpr uint32_t kPiWords[kPLength + kSLength] = {
    0x243f6a88u, 0x85a308d3u, 0x13198a2eu, 0x03707344u, 0xa4093822u, 0x299f31d0u,
    0x082efa98u, 0xec4e6c89u, 0x452821e6u, 0x38d01377u, 0xbe5466cfu, 0x34e90c6cu,
    0xc0ac29b7u, 0xc97c50ddu, 0x3f84d5b5u, 0xb5470917u, 0x9216d5d9u, 0x8979fb1bu,
    0xd1310ba6u, 0x98dfb5acu, 0x2ffd72dbu, 0xd01adfb7u, 0xb8e1afedu, 0x6a267e96u,
    0xba7c9045u, 0xf12c7f99u, 0x24a19947u, 0xb3916cf7u, 0x0801f2e2u, 0x858efc16u,
    0x636920d8u, 0x71574e69u, 0xa458fea3u, 0xf4933d7eu, 0x0d95748fu, 0x728eb658u,
    0x718bcd58u, 0x82154aeeu, 0x7b54a41du, 0xc25a59b5u, 0x9c30d539u, 0x2af26013u,
    0xc5d1b023u, 0x286085f0u, 0xca417918u, 0xb8db38efu, 0x8e79dcb0u, 0x603a180eu,
    0x6c9e0e8bu, 0xb01e8a3eu, 0xd71577c1u, 0xbd314b27u, 0x78af2fdau, 0x55605c60u,
    0xe65525f3u, 0xaa55ab94u, 0x57489862u, 0x63e81440u, 0x55ca396au, 0x2aab10b6u,
    0xb4cc5c34u, 0x1141e8ceu, 0xa15486afu, 0x7c72e993u, 0xb3ee1411u, 0x636fbc2au,
    0x2ba9c55du, 0x741831f6u, 0xce5c3e16u, 0x9b87931eu, 0xafd6ba33u, 0x6c24cf5cu,
    0x7a325381u, 0x28958677u, 0x3b8f4898u, 0x6b4bb9afu, 0xc4bfe81bu, 0x66282193u,
    0x61d809ccu, 0xfb21a991u, 0x487cac60u, 0x5dec8032u, 0xef845d5du, 0xe98575b1u,
    0xdc262302u, 0xeb651b88u, 0x23893e81u, 0xd396acc5u, 0x0f6d6ff3u, 0x83f44239u,
    0x2e0b4482u, 0xa4842004u, 0x69c8f04au, 0x9e1f9b5eu, 0x21c66842u, 0xf6e96c9au,
    0x670c9c61u, 0xabd388f0u, 0x6a51a0d2u, 0xd8542f68u, 0x960fa728u, 0xab5133a3u,
    0x6eef0b6cu, 0x137a3be4u, 0xba3bf050u, 0x7efb2a98u, 0xa1f1651du, 0x39af0176u,
    0x66ca593eu, 0x82430e88u, 0x8cee8619u, 0x456f9fb4u, 0x7d84a5c3u, 0x3b8b5ebeu,
    0xe06f75d8u, 0x85c12073u, 0x401a449fu, 0x56c16aa6u, 0x4ed3aa62u, 0x363f7706u,
    0x1bfedf72u, 0x429b023du, 0x37d0d724u, 0xd00a1248u, 0xdb0fead3u, 0x49f1c09bu,
    0x075372c9u, 0x80991b7bu, 0x25d479d8u, 0xf6e8def7u, 0xe3fe501au, 0xb6794c3bu,
    0x976ce0bdu, 0x04c006bau, 0xc1a94fb6u, 0x409f60c4u, 0x5e5c9ec2u, 0x196a2463u,
    0x68fb6fafu, 0x3e6c53b5u, 0x1339b2ebu, 0x3b52ec6fu, 0x6dfc511fu, 0x9b30952cu,
    0xcc814544u, 0xaf5ebd09u, 0xbee3d004u, 0xde334afdu, 0x660f2807u, 0x192e4bb3u,
    0xc0cba857u, 0x45c8740fu, 0xd20b5f39u, 0xb9d3fbdbu, 0x5579c0bdu, 0x1a60320au,
    0xd6a100c6u, 0x402c7279u, 0x679f25feu, 0xfb1fa3ccu, 0x8ea5e9f8u, 0xdb3222f8u,
    0x3c7516dfu, 0xfd616b15u, 0x2f501ec8u, 0xad0552abu, 0x323db5fau, 0xfd238760u,
    0x53317b48u, 0x3e00df82u, 0x9e5c57bbu, 0xca6f8ca0u, 0x1a87562eu, 0xdf1769dbu,
    0xd542a8f6u, 0x287effc3u, 0xac6732c6u, 0x8c4f5573u, 0x695b27b0u, 0xbbca58c8u,
    0xe1ffa35du, 0xb8f011a0u, 0x10fa3d98u, 0xfd2183b8u, 0x4afcb56cu, 0x2dd1d35bu,
    0x9a53e479u, 0xb6f84565u, 0xd28e49bcu, 0x4bfb9790u, 0xe1ddf2dau, 0xa4cb7e33u,
    0x62fb1341u, 0xcee4c6e8u, 0xef20cadau, 0x36774c01u, 0xd07e9efeu, 0x2bf11fb4u,
    0x95dbda4du, 0xae909198u, 0xeaad8e71u, 0x6b93d5a0u, 0xd08ed1d0u, 0xafc725e0u,
    0x8e3c5b2fu, 0x8e7594b7u, 0x8ff6e2fbu, 0xf2122b64u, 0x8888b812u, 0x900df01cu,
    0x4fad5ea0u, 0x688fc31cu, 0xd1cff191u, 0xb3a8c1adu, 0x2f2f2218u, 0xbe0e1777u,
    0xea752dfeu, 0x8b021fa1u, 0xe5a0cc0fu, 0xb56f74e8u, 0x18acf3d6u, 0xce89e299u,
    0xb4a84fe0u, 0xfd13e0b7u, 0x7cc43b81u, 0xd2ada8d9u, 0x165fa266u, 0x80957705u,
    0x93cc7314u, 0x211a1477u, 0xe6ad2065u, 0x77b5fa86u, 0xc75442f5u, 0xfb9d35cfu,
    0xebcdaf0cu, 0x7b3e89a0u, 0xd6411bd3u, 0xae1e7e49u, 0x00250e2du, 0x2071b35eu,
    0x226800bbu, 0x57b8e0afu, 0x2464369bu, 0xf009b91eu, 0x5563911du, 0x59dfa6aau,
    0x78c14389u, 0xd95a537fu, 0x207d5ba2u, 0x02e5b9c5u, 0x83260376u, 0x6295cfa9u,
    0x11c81968u, 0x4e734a41u, 0xb3472dcau, 0x7b14a94au, 0x1b510052u, 0x9a532915u,
    0xd60f573fu, 0xbc9bc6e4u, 0x2b60a476u, 0x81e67400u, 0x08ba6fb5u, 0x571be91fu,
    0xf296ec6bu, 0x2a0dd915u, 0xb6636521u, 0xe7b9f9b6u, 0xff34052eu, 0xc5855664u,
    0x53b02d5du, 0xa99f8fa1u, 0x08ba4799u, 0x6e85076au, 0x4b7a70e9u, 0xb5b32944u,
    0xdb75092eu, 0xc4192623u, 0xad6ea6b0u, 0x49a7df7du, 0x9cee60b8u, 0x8fedb266u,
    0xecaa8c71u, 0x699a17ffu, 0x5664526cu, 0xc2b19ee1u, 0x193602a5u, 0x75094c29u,
    0xa0591340u, 0xe4183a3eu, 0x3f54989au, 0x5b429d65u, 0x6b8fe4d6u, 0x99f73fd6u,
    0xa1d29c07u, 0xefe830f5u, 0x4d2d38e6u, 0xf0255dc1u, 0x4cdd2086u, 0x8470eb26u,
    0x6382e9c6u, 0x021ecc5eu, 0x09686b3fu, 0x3ebaefc9u, 0x3c971814u, 0x6b6a70a1u,
    0x687f3584u, 0x52a0e286u, 0xb79c5305u, 0xaa500737u, 0x3e07841cu, 0x7fdeae5cu,
    0x8e7d44ecu, 0x5716f2b8u, 0xb03ada37u, 0xf0500c0du, 0xf01c1f04u, 0x0200b3ffu,
    0xae0cf51au, 0x3cb574b2u, 0x25837a58u, 0xdc0921bdu, 0xd19113f9u, 0x7ca92ff6u,
    0x94324773u, 0x22f54701u, 0x3ae5e581u, 0x37c2dadcu, 0xc8b57634u, 0x9af3dda7u,
    0xa9446146u, 0x0fd0030eu, 0xecc8c73eu, 0xa4751e41u, 0xe238cd99u, 0x3bea0e2fu,
    0x3280bba1u, 0x183eb331u, 0x4e548b38u, 0x4f6db908u, 0x6f420d03u, 0xf60a04bfu,
    0x2cb81290u, 0x24977c79u, 0x5679b072u, 0xbcaf89afu, 0xde9a771fu, 0xd9930810u,
    0xb38bae12u, 0xdccf3f2eu, 0x5512721fu, 0x2e6b7124u, 0x501adde6u, 0x9f84cd87u,
    0x7a584718u, 0x7408da17u, 0xbc9f9abcu, 0xe94b7d8cu, 0xec7aec3au, 0xdb851dfau,
    0x63094366u, 0xc464c3d2u, 0xef1c1847u, 0x3215d908u, 0xdd433b37u, 0x24c2ba16u,
    0x12a14d43u, 0x2a65c451u, 0x50940002u, 0x133ae4ddu, 0x71dff89eu, 0x10314e55u,
    0x81ac77d6u, 0x5f11199bu, 0x043556f1u, 0xd7a3c76bu, 0x3c11183bu, 0x5924a509u,
    0xf28fe6edu, 0x97f1fbfau, 0x9ebabf2cu, 0x1e153c6eu, 0x86e34570u, 0xeae96fb1u,
    0x860e5e0au, 0x5a3e2ab3u, 0x771fe71cu, 0x4e3d06fau, 0x2965dcb9u, 0x99e71d0fu,
    0x803e89d6u, 0x5266c825u, 0x2e4cc978u, 0x9c10b36au, 0xc6150ebau, 0x94e2ea78u,
    0xa5fc3c53u, 0x1e0a2df4u, 0xf2f74ea7u, 0x361d2b3du, 0x1939260fu, 0x19c27960u,
    0x5223a708u, 0xf71312b6u, 0xebadfe6eu, 0xeac31f66u, 0xe3bc4595u, 0xa67bc883u,
    0xb17f37d1u, 0x018cff28u, 0xc332ddefu, 0xbe6c5aa5u, 0x65582185u, 0x68ab9802u,
    0xeecea50fu, 0xdb2f953bu, 0x2aef7dadu, 0x5b6e2f84u, 0x1521b628u, 0x29076170u,
    0xecdd4775u, 0x619f1510u, 0x13cca830u, 0xeb61bd96u, 0x0334fe1eu, 0xaa0363cfu,
    0xb5735c90u, 0x4c70a239u, 0xd59e9e0bu, 0xcbaade14u, 0xeecc86bcu, 0x60622ca7u,
    0x9cab5cabu, 0xb2f3846eu, 0x648b1eafu, 0x19bdf0cau, 0xa02369b9u, 0x655abb50u,
    0x40685a32u, 0x3c2ab4b3u, 0x319ee9d5u, 0xc021b8f7u, 0x9b540b19u, 0x875fa099u,
    0x95f7997eu, 0x623d7da8u, 0xf837889au, 0x97e32d77u, 0x11ed935fu, 0x16681281u,
    0x0e358829u, 0xc7e61fd6u, 0x96dedfa1u, 0x7858ba99u, 0x57f584a5u, 0x1b227263u,
    0x9b83c3ffu, 0x1ac24696u, 0xcdb30aebu, 0x532e3054u, 0x8fd948e4u, 0x6dbc3128u,
    0x58ebf2efu, 0x34c6ffeau, 0xfe28ed61u, 0xee7c3c73u, 0x5d4a14d9u, 0xe864b7e3u,
    0x42105d14u, 0x203e13e0u, 0x45eee2b6u, 0xa3aaabeau, 0xdb6c4f15u, 0xfacb4fd0u,
    0xc742f442u, 0xef6abbb5u, 0x654f3b1du, 0x41cd2105u, 0xd81e799eu, 0x86854dc7u,
    0xe44b476au, 0x3d816250u, 0xcf62a1f2u, 0x5b8d2646u, 0xfc8883a0u, 0xc1c7b6a3u,
    0x7f1524c3u, 0x69cb7492u, 0x47848a0bu, 0x5692b285u, 0x095bbf00u, 0xad19489du,
    0x1462b174u, 0x23820e00u, 0x58428d2au, 0x0c55f5eau, 0x1dadf43eu, 0x233f7061u,
    0x3372f092u, 0x8d937e41u, 0xd65fecf1u, 0x6c223bdbu, 0x7cde3759u, 0xcbee7460u,
    0x4085f2a7u, 0xce77326eu, 0xa6078084u, 0x19f8509eu, 0xe8efd855u, 0x61d99735u,
    0xa969a7aau, 0xc50c06c2u, 0x5a04abfcu, 0x800bcadcu, 0x9e447a2eu, 0xc3453484u,
    0xfdd56705u, 0x0e1e9ec9u, 0xdb73dbd3u, 0x105588cdu, 0x675fda79u, 0xe3674340u,
    0xc5c43465u, 0x713e38d8u, 0x3d28f89eu, 0xf16dff20u, 0x153e21e7u, 0x8fb03d4au,
    0xe6e39f2bu, 0xdb83adf7u, 0xe93d5a68u, 0x948140f7u, 0xf64c261cu, 0x94692934u,
    0x411520f7u, 0x7602d4f7u, 0xbcf46b2eu, 0xd4a20068u, 0xd4082471u, 0x3320f46au,
    0x43b7d4b7u, 0x500061afu, 0x1e39f62eu, 0x97244546u, 0x14214f74u, 0xbf8b8840u,
    0x4d95fc1du, 0x96b591afu, 0x70f4ddd3u, 0x66a02f45u, 0xbfbc09ecu, 0x03bd9785u,
    0x7fac6dd0u, 0x31cb8504u, 0x96eb27b3u, 0x55fd3941u, 0xda2547e6u, 0xabca0a9au,
    0x28507825u, 0x530429f4u, 0x0a2c86dau, 0xe9b66dfbu, 0x68dc1462u, 0xd7486900u,
    0x680ec0a4u, 0x27a18deeu, 0x4f3ffea2u, 0xe887ad8cu, 0xb58ce006u, 0x7af4d6b6u,
    0xaace1e7cu, 0xd3375fecu, 0xce78a399u, 0x406b2a42u, 0x20fe9e35u, 0xd9f385b9u,
    0xee39d7abu, 0x3b124e8bu, 0x1dc9faf7u, 0x4b6d1856u, 0x26a36631u, 0xeae397b2u,
    0x3a6efa74u, 0xdd5b4332u, 0x6841e7f7u, 0xca7820fbu, 0xfb0af54eu, 0xd8feb397u,
    0x454056acu, 0xba489527u, 0x55533a3au, 0x20838d87u, 0xfe6ba9b7u, 0xd096954bu,
    0x55a867bcu, 0xa1159a58u, 0xcca92963u, 0x99e1db33u, 0xa62a4a56u, 0x3f3125f9u,
    0x5ef47e1cu, 0x9029317cu, 0xfdf8e802u, 0x04272f70u, 0x80bb155cu, 0x05282ce3u,
    0x95c11548u, 0xe4c66d22u, 0x48c1133fu, 0xc70f86dcu, 0x07f9c9eeu, 0x41041f0fu,
    0x404779a4u, 0x5d886e17u, 0x325f51ebu, 0xd59bc0d1u, 0xf2bcc18fu, 0x41113564u,
    0x257b7834u, 0x602a9c60u, 0xdff8e8a3u, 0x1f636c1bu, 0x0e12b4c2u, 0x02e1329eu,
    0xaf664fd1u, 0xcad18115u, 0x6b2395e0u, 0x333e92e1u, 0x3b240b62u, 0xeebeb922u,
    0x85b2a20eu, 0xe6ba0d99u, 0xde720c8cu, 0x2da2f728u, 0xd0127845u, 0x95b794fdu,
    0x647d0862u, 0xe7ccf5f0u, 0x5449a36fu, 0x877d48fau, 0xc39dfd27u, 0xf33e8d1eu,
    0x0a476341u, 0x992eff74u, 0x3a6f6eabu, 0xf4f8fd37u, 0xa812dc60u, 0xa1ebddf8u,
    0x991be14cu, 0xdb6e6b0du, 0xc67b5510u, 0x6d672c37u, 0x2765d43bu, 0xdcd0e804u,
    0xf1290dc7u, 0xcc00ffa3u, 0xb5390f92u, 0x690fed0bu, 0x667b9ffbu, 0xcedb7d9cu,
    0xa091cf0bu, 0xd9155ea3u, 0xbb132f88u, 0x515bad24u, 0x7b9479bfu, 0x763bd6ebu,
    0x37392eb3u, 0xcc115979u, 0x8026e297u, 0xf42e312du, 0x6842ada7u, 0xc66a2b3bu,
    0x12754cccu, 0x782ef11cu, 0x6a124237u, 0xb79251e7u, 0x06a1bbe6u, 0x4bfb6350u,
    0x1a6b1018u, 0x11caedfau, 0x3d25bdd8u, 0xe2e1c3c9u, 0x44421659u, 0x0a121386u,
    0xd90cec6eu, 0xd5abea2au, 0x64af674eu, 0xda86a85fu, 0xbebfe988u, 0x64e4c3feu,
    0x9dbc8057u, 0xf0f7c086u, 0x60787bf8u, 0x6003604du, 0xd1fd8346u, 0xf6381fb0u,
    0x7745ae04u, 0xd736fcccu, 0x83426b33u, 0xf01eab71u, 0xb0804187u, 0x3c005e5fu,
    0x77a057beu, 0xbde8ae24u, 0x55464299u, 0xbf582e61u, 0x4e58f48fu, 0xf2ddfda2u,
    0xf474ef38u, 0x8789bdc2u, 0x5366f9c3u, 0xc8b38e74u, 0xb475f255u, 0x46fcd9b9u,
    0x7aeb2661u, 0x8b1ddf84u, 0x846a0e79u, 0x915f95e2u, 0x466e598eu, 0x20b45770u,
    0x8cd55591u, 0xc902de4cu, 0xb90bace1u, 0xbb8205d0u, 0x11a86248u, 0x7574a99eu,
    0xb77f19b6u, 0xe0a9dc09u, 0x662d09a1u, 0xc4324633u, 0xe85a1f02u, 0x09f0be8cu,
    0x4a99a025u, 0x1d6efe10u, 0x1ab93d1du, 0x0ba5a4dfu, 0xa186f20fu, 0x2868f169u,
    0xdcb7da83u, 0x573906feu, 0xa1e2ce9bu, 0x4fcd7f52u, 0x50115e01u, 0xa70683fau,
    0xa002b5c4u, 0x0de6d027u, 0x9af88c27u, 0x773f8641u, 0xc3604c06u, 0x61a806b5u,
    0xf0177a28u, 0xc0f586e0u, 0x006058aau, 0x30dc7d62u, 0x11e69ed7u, 0x2338ea63u,
    0x53c2dd94u, 0xc2c21634u, 0xbbcbee56u, 0x90bcb6deu, 0xebfc7da1u, 0xce591d76u,
    0x6f05e409u, 0x4b7c0188u, 0x39720a3du, 0x7c927c24u, 0x86e3725fu, 0x724d9db9u,
    0x1ac15bb4u, 0xd39eb8fcu, 0xed545578u, 0x08fca5b5u, 0xd83d7cd3u, 0x4dad0fc4u,
    0x1e50ef5eu, 0xb161e6f8u, 0xa28514d9u, 0x6c51133cu, 0x6fd5c7e7u, 0x56e14ec4u,
    0x362abfceu, 0xddc6c837u, 0xd79a3234u, 0x92638212u, 0x670efa8eu, 0x406000e0u,
    0x3a39ce37u, 0xd3faf5cfu, 0xabc27737u, 0x5ac52d1bu, 0x5cb0679eu, 0x4fa33742u,
    0xd3822740u, 0x99bc9bbeu, 0xd5118e9du, 0xbf0f7315u, 0xd62d1c7eu, 0xc700c47bu,
    0xb78c1b6bu, 0x21a19045u, 0xb26eb1beu, 0x6a366eb4u, 0x5748ab2fu, 0xbc946e79u,
    0xc6a376d2u, 0x6549c2c8u, 0x530ff8eeu, 0x468dde7du, 0xd5730a1du, 0x4cd04dc6u,
    0x2939bbdbu, 0xa9ba4650u, 0xac9526e8u, 0xbe5ee304u, 0xa1fad5f0u, 0x6a2d519au,
    0x63ef8ce2u, 0x9a86ee22u, 0xc089c2b8u, 0x43242ef6u, 0xa51e03aau, 0x9cf2d0a4u,
    0x83c061bau, 0x9be96a4du, 0x8fe51550u, 0xba645bd6u, 0x2826a2f9u, 0xa73a3ae1u,
    0x4ba99586u, 0xef5562e9u, 0xc72fefd3u, 0xf752f7dau, 0x3f046f69u, 0x77fa0a59u,
    0x80e4a915u, 0x87b08601u, 0x9b09e6adu, 0x3b3ee593u, 0xe990fd5au, 0x9e34d797u,
    0x2cf0b7d9u, 0x022b8b51u, 0x96d5ac3au, 0x017da67du, 0xd1cf3ed6u, 0x7c7d2d28u,
    0x1f9f25cfu, 0xadf2b89bu, 0x5ad6b472u, 0x5a88f54cu, 0xe029ac71u, 0xe019a5e6u,
    0x47b0acfdu, 0xed93fa9bu, 0xe8d3c48du, 0x283b57ccu, 0xf8d56629u, 0x79132e28u,
    0x785f0191u, 0xed756055u, 0xf7960e44u, 0xe3d35e8cu, 0x15056dd4u, 0x88f46dbau,
    0x03a16125u, 0x0564f0bdu, 0xc3eb9e15u, 0x3c9057a2u, 0x97271aecu, 0xa93a072au,
    0x1b3f6d9bu, 0x1e6321f5u, 0xf59c66fbu, 0x26dcf319u, 0x7533d928u, 0xb155fdf5u,
    0x03563482u, 0x8aba3cbbu, 0x28517711u, 0xc20ad9f8u, 0xabcc5167u, 0xccad925fu,
    0x4de81751u, 0x3830dc8eu, 0x379d5862u, 0x9320f991u, 0xea7a90c2u, 0xfb3e7bceu,
    0x5121ce64u, 0x774fbe32u, 0xa8b6e37eu, 0xc3293d46u, 0x48de5369u, 0x6413e680u,
    0xa2ae0810u, 0xdd6db224u, 0x69852dfdu, 0x09072166u, 0xb39a460au, 0x6445c0ddu,
    0x586cdecfu, 0x1c20c8aeu, 0x5bbef7ddu, 0x1b588d40u, 0xccd2017fu, 0x6bb4e3bbu,
    0xdda26a7eu, 0x3a59ff45u, 0x3e350a44u, 0xbcb4cdd5u, 0x72eacea8u, 0xfa6484bbu,
    0x8d6612aeu, 0xbf3c6f47u, 0xd29be463u, 0x542f5d9eu, 0xaec2771bu, 0xf64e6370u,
    0x740e0d8du, 0xe75b1357u, 0xf8721671u, 0xaf537d5du, 0x4040cb08u, 0x4eb4e2ccu,
    0x34d2466au, 0x0115af84u, 0xe1b00428u, 0x95983a1du, 0x06b89fb4u, 0xce6ea048u,
    0x6f3f3b82u, 0x3520ab82u, 0x011a1d4bu, 0x277227f8u, 0x611560b1u, 0xe7933fdcu,
    0xbb3a792bu, 0x344525bdu, 0xa08839e1u, 0x51ce794bu, 0x2f32c9b7u, 0xa01fbac9u,
    0xe01cc87eu, 0xbcc7d1f6u, 0xcf0111c3u, 0xa1e8aac7u, 0x1a908749u, 0xd44fbd9au,
    0xd0dadecbu, 0xd50ada38u, 0x0339c32au, 0xc6913667u, 0x8df9317cu, 0xe0b12b4fu,
    0xf79e59b7u, 0x43f5bb3au, 0xf2d519ffu, 0x27d9459cu, 0xbf97222cu, 0x15e6fc2au,
    0x0f91fc71u, 0x9b941525u, 0xfae59361u, 0xceb69cebu, 0xc2a86459u, 0x12baa8d1u,
    0xb6c1075eu, 0xe3056a0cu, 0x10d25065u, 0xcb03a442u, 0xe0ec6e0eu, 0x1698db3bu,
    0x4c98a0beu, 0x3278e964u, 0x9f1f9532u, 0xe0d392dfu, 0xd3a0342bu, 0x8971f21eu,
    0x1b0a7441u, 0x4ba3348cu, 0xc5be7120u, 0xc37632d8u, 0xdf359f8du, 0x9b992f2eu,
    0xe60b6f47u, 0x0fe3f11du, 0xe54cda54u, 0x1edad891u, 0xce6279cfu, 0xcd3e7e6fu,
    0x1618b166u, 0xfd2c1d05u, 0x848fd2c5u, 0xf6fb2299u, 0xf523f357u, 0xa6327623u,
    0x93a83531u, 0x56cccd02u, 0xacf08162u, 0x5a75ebb5u, 0x6e163697u, 0x88d273ccu,
    0xde966292u, 0x81b949d0u, 0x4c50901bu, 0x71c65614u, 0xe6c6c7bdu, 0x327a140au,
    0x45e1d006u, 0xc3f27b9au, 0xc9aa53fdu, 0x62a80f00u, 0xbb25bfe2u, 0x35bdd2f6u,
    0x71126905u, 0xb2040222u, 0xb6cbcf7cu, 0xcd769c2bu, 0x53113ec0u, 0x1640e3d3u,
    0x38abbd60u, 0x2547adf0u, 0xba38209cu, 0xf746ce76u, 0x77afa1c5u, 0x20756060u,
    0x85cbfe4eu, 0x8ae88dd8u, 0x7aaaf9b0u, 0x4cf9aa7eu, 0x1948c25cu, 0x02fb8a8cu,
    0x01c36ae4u, 0xd6ebe1f9u, 0x90d4f869u, 0xa65cdea0u, 0x3f09252du, 0xc208e69fu,
    0xb74e6132u, 0xce77e25bu, 0x578fdfe3u, 0x3ac372e6u,
};

void init_state(BF_KEY& state) {
  for (int i = 0; i < kPLength; ++i) state.P[i] = static_cast<BF_LONG>(kPiWords[i]);
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 256; ++j) {
      // BF_KEY::S 是【一维】的 256*4 个字（OpenSSL 的声明），不是 [4][256]
      state.S[i * 256 + j] = static_cast<BF_LONG>(kPiWords[kPLength + i * 256 + j]);
    }
  }
}

// 从字节流里取 4 个字节拼成一个 32 位字（**大端**），游标循环回绕。
// len == 0 时一律取 data[0]（等价于"1 字节的 \0 流"）—— 这正是空口令的形态：
// 参考实现里 key 指向 "" 这个字符串的下标 0，即那个 NUL。调用方保证 data[0] 可读。
uint32_t stream2word(const unsigned char* data, std::size_t len, std::size_t* cursor) {
  uint32_t word = 0;
  std::size_t j = *cursor;
  for (int i = 0; i < 4; ++i, ++j) {
    if (j >= len) j = 0;
    word = (word << 8) | data[j];
  }
  *cursor = j;
  return word;
}

// 用当前 P/S 状态加密一个 64 位块（Blowfish 是对称的，加密即解密）。
void encipher(const BF_KEY& state, uint32_t* left, uint32_t* right) {
  BF_LONG block[2] = {static_cast<BF_LONG>(*left), static_cast<BF_LONG>(*right)};
  BF_encrypt(block, &state);
  *left = static_cast<uint32_t>(block[0]);
  *right = static_cast<uint32_t>(block[1]);
}

// Blowfish_expand0state(c, key)：只有 key 参与 P 的异或，encipher 流不加盐。
// ExpandKey(state, 0, key) 与 ExpandKey(state, 0, salt) 都落到这里。
void expand0state(BF_KEY& state, const unsigned char* key, std::size_t key_len) {
  std::size_t cursor = 0;
  for (int i = 0; i < kPLength; ++i) {
    state.P[i] = static_cast<BF_LONG>(static_cast<uint32_t>(state.P[i]) ^
                                      stream2word(key, key_len, &cursor));
  }
  uint32_t l = 0;
  uint32_t r = 0;
  for (int i = 0; i < kPLength; i += 2) {
    encipher(state, &l, &r);
    state.P[i] = static_cast<BF_LONG>(l);
    state.P[i + 1] = static_cast<BF_LONG>(r);
  }
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 256; j += 2) {
      encipher(state, &l, &r);
      state.S[i * 256 + j] = static_cast<BF_LONG>(l);
      state.S[i * 256 + j + 1] = static_cast<BF_LONG>(r);
    }
  }
}

// Blowfish_expandstate(c, salt, key)：密钥异或 P 之后，encipher 流每个块还要异或盐的
// 【两个】32 位字（分别进 l 与 r）。bcrypt 里只有第一次 ExpandKey 带盐。
// ⚠️ 【两个而不是一个】—— 这是本实现最容易与参考实现对不上的地方，且症状是
//   "哈希与任何其他工具都不同"而不是崩溃。本轮实测（pyca/bcrypt 3.2.2 的
//   Blowfish_expandstate 反汇编：0x32ab 与 0x331b 两次 xor 分别作用于 lr[0] 与 lr[1]）
//   确认 P 循环与 S 循环每次 encipher 各消耗两个盐字、游标连续。
//   写成"每次一个、l/r 交替"（OpenBSD 早期文献的写法）会算出完全不同的哈希。
void expandstate(BF_KEY& state, const unsigned char* salt, const unsigned char* key,
                 std::size_t key_len) {
  std::size_t cursor = 0;
  for (int i = 0; i < kPLength; ++i) {
    state.P[i] = static_cast<BF_LONG>(static_cast<uint32_t>(state.P[i]) ^
                                      stream2word(key, key_len, &cursor));
  }
  std::size_t salt_cursor = 0;
  uint32_t l = 0;
  uint32_t r = 0;
  for (int i = 0; i < kPLength; i += 2) {
    l ^= stream2word(salt, kBcryptSaltBytes, &salt_cursor);
    r ^= stream2word(salt, kBcryptSaltBytes, &salt_cursor);
    encipher(state, &l, &r);
    state.P[i] = static_cast<BF_LONG>(l);
    state.P[i + 1] = static_cast<BF_LONG>(r);
  }
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 256; j += 2) {
      l ^= stream2word(salt, kBcryptSaltBytes, &salt_cursor);
      r ^= stream2word(salt, kBcryptSaltBytes, &salt_cursor);
      encipher(state, &l, &r);
      state.S[i * 256 + j] = static_cast<BF_LONG>(l);
      state.S[i * 256 + j + 1] = static_cast<BF_LONG>(r);
    }
  }
}

// EksBlowfishSetup(cost, salt, key) + 64 轮加密魔文 → 24 字节密文。
std::string eks_blowfish(const unsigned char* salt, const unsigned char* key,
                         std::size_t key_len, int cost) {
  BF_KEY state;
  init_state(state);
  expandstate(state, salt, key, key_len);
  const std::uint64_t rounds = std::uint64_t{1} << cost;
  for (std::uint64_t i = 0; i < rounds; ++i) {
    expand0state(state, key, key_len);
    expand0state(state, salt, kBcryptSaltBytes);
  }

  // "OrpheanBeholderScryDoubt" 按大端拆成 6 个 32 位字（3 个 64 位块）
  static const char kMagic[24] = {'O', 'r', 'p', 'h', 'e', 'a', 'n', 'B', 'e', 'h', 'o', 'l',
                                  'd', 'e', 'r', 'S', 'c', 'r', 'y', 'D', 'o', 'u', 'b', 't'};
  uint32_t block[6];
  for (int i = 0; i < 6; ++i) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(kMagic) + i * 4;
    block[i] = (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
  }
  for (int i = 0; i < 64; ++i) {
    for (int j = 0; j < 3; ++j) {
      uint32_t left = block[j * 2];
      uint32_t right = block[j * 2 + 1];
      encipher(state, &left, &right);
      block[j * 2] = left;
      block[j * 2 + 1] = right;
    }
  }
  std::string out(24, '\0');
  for (int i = 0; i < 6; ++i) {
    unsigned char* p = reinterpret_cast<unsigned char*>(&out[i * 4]);
    p[0] = static_cast<unsigned char>(block[i] >> 24);
    p[1] = static_cast<unsigned char>(block[i] >> 16);
    p[2] = static_cast<unsigned char>(block[i] >> 8);
    p[3] = static_cast<unsigned char>(block[i]);
  }
  return out;
}

// ---- bcrypt 自己的 base64（字母表顺序是 R-20 的头号坑）--------------------------
// 注意不是标准 base64 的 A-Za-z0-9+/，而是 ./A-Za-z0-9。写错时程序不会崩，
// 只是全部口令都验不过（或全部通过），因此靠 tests 里的参考向量钉死。
constexpr char kBase64[] = "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

// 参考实现的写法：逐字节推进，末尾不足 3 字节时补 0 位凑满最后一组。
// 对 24 字节密文产出 32 字符，调用方【只取前 31 个】。
std::string base64_encode(const unsigned char* data, std::size_t len) {
  std::string out;
  out.reserve((len + 2) / 3 * 4);
  std::size_t i = 0;
  while (i < len) {
    const unsigned int c1 = data[i++];
    out.push_back(kBase64[(c1 >> 2) & 0x3F]);
    unsigned int acc = (c1 & 0x03) << 4;
    if (i == len) {  // 补一个 0 位
      out.push_back(kBase64[acc & 0x3F]);
      break;
    }
    const unsigned int c2 = data[i++];
    acc |= (c2 >> 4);
    out.push_back(kBase64[acc & 0x3F]);
    acc = (c2 & 0x0F) << 2;
    if (i == len) {
      out.push_back(kBase64[acc & 0x3F]);
      break;
    }
    const unsigned int c3 = data[i++];
    acc |= (c3 >> 6);
    out.push_back(kBase64[acc & 0x3F]);
    out.push_back(kBase64[c3 & 0x3F]);
  }
  return out;
}

int base64_value(char c) {
  for (int i = 0; i < 64; ++i) {
    if (kBase64[i] == c) return i;
  }
  return -1;
}

// 22 个字符 → 16 字节。末 2 个字符只贡献 1 个字节（低 4 位丢弃），与参考实现一致。
bool base64_decode_salt(const std::string& text, unsigned char out[kBcryptSaltBytes]) {
  if (text.size() != kBcryptSaltChars) return false;
  std::size_t i = 0;
  for (int group = 0; group < 5; ++group) {  // 5 组 × 3 字节 = 15
    unsigned int word = 0;
    for (int k = 0; k < 4; ++k) {
      const int v = base64_value(text[i++]);
      if (v < 0) return false;
      word = (word << 6) | static_cast<unsigned int>(v);
    }
    out[group * 3] = static_cast<unsigned char>(word >> 16);
    out[group * 3 + 1] = static_cast<unsigned char>(word >> 8);
    out[group * 3 + 2] = static_cast<unsigned char>(word);
  }
  const int v0 = base64_value(text[i++]);
  const int v1 = base64_value(text[i]);
  if (v0 < 0 || v1 < 0) return false;
  out[15] = static_cast<unsigned char>(((v0 << 2) | (v1 >> 4)) & 0xFF);
  return true;
}

// ---- 解析哈希串 ----------------------------------------------------------------
struct ParsedHash {
  char minor = 'b';   // '$2a$' / '$2b$' / '$2y$'
  int cost = 0;
  unsigned char salt[kBcryptSaltBytes] = {};
  std::string digest;  // 31 个字符
};

bool parse_hash(const std::string& stored, ParsedHash* out) {
  if (stored.size() != kBcryptHashLength) return false;
  // "$2" + 版本 + "$" + 2 位 cost + "$" = 7 个前缀字符
  if (stored[0] != '$' || stored[1] != '2' || stored[3] != '$' || stored[6] != '$') return false;
  // 只认 a/b/y：$2$ 是 1997 年的老变体（密钥流不含结尾 NUL），本项目不产生；
  // $2x$ 只用于标记"由有符号扩展 bug 的实现生成"的哈希，也不认（见 verify_password）。
  if (stored[2] != 'a' && stored[2] != 'b' && stored[2] != 'y') return false;
  if (stored[4] < '0' || stored[4] > '9' || stored[5] < '0' || stored[5] > '9') return false;
  const int cost = (stored[4] - '0') * 10 + (stored[5] - '0');
  if (cost < kBcryptMinCost || cost > kBcryptMaxCost) return false;

  for (char c : stored.substr(7, kBcryptSaltChars)) {
    if (base64_value(c) < 0) return false;
  }
  for (char c : stored.substr(7 + kBcryptSaltChars, kBcryptDigestChars)) {
    if (base64_value(c) < 0) return false;
  }
  if (!base64_decode_salt(stored.substr(7, kBcryptSaltChars), out->salt)) return false;
  out->minor = stored[2];
  out->cost = cost;
  out->digest = stored.substr(7 + kBcryptSaltChars, kBcryptDigestChars);
  return true;
}

// 密钥长度：min(strlen, 72) + 1（末尾的 NUL 参与密钥流）。
// ⚠️ strlen 而非 std::string::size()，理由见文件头。空口令时返回 1，
//   而 expand0state 在 key_len==0 时会反复读 data[0] —— 两者等价，
//   但统一走 +1 的分支可以避免"空口令时是否越界读"这个问题出现在代码里。
std::size_t key_length_of(const std::string& password) {
  const std::size_t raw = std::strlen(password.c_str());
  const std::size_t capped = raw > kBcryptMaxKeyBytes ? kBcryptMaxKeyBytes : raw;
  return capped + 1;  // 含结尾 NUL
}

// 空口令用的 1 字节缓冲区：expand0state 的 stream2word 在 key_len==0 时读 data[0]，
// 给它一个真实的 \0 就与参考实现（key 指向 "" 的第 0 字节）完全一致。
constexpr unsigned char kEmptyKey[1] = {0};

}  // namespace

// ---- 对外接口 -------------------------------------------------------------------

bool is_hash_wellformed(const std::string& stored) {
  ParsedHash parsed;
  return parse_hash(stored, &parsed);
}

std::optional<int> cost_of_hash(const std::string& stored) {
  ParsedHash parsed;
  if (!parse_hash(stored, &parsed)) return std::nullopt;
  return parsed.cost;
}

std::string hash_password(const std::string& password, int cost) {
  if (cost < kBcryptMinCost || cost > kBcryptMaxCost) {
    throw std::invalid_argument("bcrypt 的 cost 必须在 [4,31]，收到 " + std::to_string(cost));
  }
  unsigned char salt[kBcryptSaltBytes];
  if (RAND_bytes(salt, static_cast<int>(kBcryptSaltBytes)) != 1) {
    // 与 session::generate_token 同一判断：熵源不可用时抛，而不是退化成可预测的盐。
    throw std::runtime_error("RAND_bytes 不可用，拒绝用可预测的盐做密码哈希");
  }
  const std::string salt_b64 = base64_encode(salt, kBcryptSaltBytes);
  return hash_password_with_salt(password, cost, salt_b64);
}

std::string hash_password_with_salt(const std::string& password, int cost,
                                    const std::string& salt_b64) {
  if (cost < kBcryptMinCost || cost > kBcryptMaxCost) {
    throw std::invalid_argument("bcrypt 的 cost 必须在 [4,31]，收到 " + std::to_string(cost));
  }
  unsigned char salt[kBcryptSaltBytes];
  if (!base64_decode_salt(salt_b64, salt)) {
    throw std::invalid_argument("盐必须是 22 个合法的 bcrypt base64 字符");
  }
  const std::size_t key_len = key_length_of(password);
  // 只保留 strlen 之前的内容（size() 可能更长，含 NUL 时更甚），末尾 NUL 由 key_len 计数。
  const std::string truncated = password.substr(0, key_len - 1);
  const unsigned char* key_bytes =
      truncated.empty() ? kEmptyKey : reinterpret_cast<const unsigned char*>(truncated.data());

  const std::string cipher = eks_blowfish(salt, key_bytes, key_len, cost);
  // ⚠️ 摘要只编码【前 23 字节】，不是"编码 24 字节再截掉最后一字符" —— 两者差在
  //   最后一个字符的 2 个比特上，而它恰恰就是哈希的最后一位（参考实现的
  //   encode_base64(ciphertext, 23) ，同样由反汇编确认）。
  const std::string encoded = base64_encode(
      reinterpret_cast<const unsigned char*>(cipher.data()), kBcryptDigestBytes);

  std::string out = kBcryptPrefix;
  out += static_cast<char>('0' + cost / 10);
  out += static_cast<char>('0' + cost % 10);
  out += '$';
  out += salt_b64;
  out += encoded;
  return out;
}

bool verify_password(const std::string& password, const std::string& stored) {
  ParsedHash parsed;
  if (!parse_hash(stored, &parsed)) return false;   // 坏数据不该让请求崩掉

  const std::size_t key_len = key_length_of(password);
  std::string truncated = password.substr(0, key_len - 1);
  const unsigned char* key_bytes =
      truncated.empty() ? kEmptyKey : reinterpret_cast<const unsigned char*>(truncated.data());
  const std::string cipher = eks_blowfish(parsed.salt, key_bytes, key_len, parsed.cost);
  const std::string digest = base64_encode(
      reinterpret_cast<const unsigned char*>(cipher.data()), kBcryptDigestBytes);
  // 定长比较：短路返回的比较在"猜对前缀"上有可测的时序差异，而这里比的是完整摘要。
  return CRYPTO_memcmp(digest.data(), parsed.digest.data(), kBcryptDigestChars) == 0;
}

}  // namespace web
}  // namespace oj
