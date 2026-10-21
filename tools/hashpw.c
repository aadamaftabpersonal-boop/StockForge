/* Dev tool: reads a password from stdin, prints its Argon2id hash (libsodium,
 * same parameters as the server). Used once to produce the demo-account hashes
 * in db/seed. Never used at runtime. */
#include <sodium.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    char pw[256], out[crypto_pwhash_STRBYTES];
    if (sodium_init() < 0 || !fgets(pw, sizeof pw, stdin)) return 1;
    pw[strcspn(pw, "\r\n")] = 0;
    if (crypto_pwhash_str(out, pw, strlen(pw), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE)) return 1;
    puts(out);
    return 0;
}
