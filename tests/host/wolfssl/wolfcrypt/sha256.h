#include <openssl/sha.h>
typedef SHA256_CTX wc_Sha256;typedef unsigned word32;
static inline int wc_InitSha256(wc_Sha256 *c){return SHA256_Init(c)==1?0:-1;}
static inline int wc_Sha256Update(wc_Sha256 *c,const unsigned char *b,word32 n){return SHA256_Update(c,b,n)==1?0:-1;}
static inline int wc_Sha256Final(wc_Sha256 *c,unsigned char *b){return SHA256_Final(b,c)==1?0:-1;}
