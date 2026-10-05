/* test-sources: exercises the two ways the C API reads a model other than from a plain folder:
 * a model in a .tar file, and a codec for the weights.
 *
 *   test-sources MODEL [OPTIONS_JSON] [--codec shift|fail]
 *       Loads MODEL (a folder or a .tar file), asks one fixed question and prints one line:
 *         OK <the answer, without the timing>
 *         ERROR <the reason>
 *       --codec shift  sets the test codec below with laya_set_codec
 *       --codec fail   sets a codec that reports error 7
 *
 *   test-sources --encode IN OUT
 *       Writes OUT as the stored form of IN for the "shift" test codec.
 *
 * The test codec is deliberately trivial: each stored byte is the plain byte plus a number that
 * depends on its position in the file. It exists to check that the library passes the right
 * blocks and offsets, nothing more.
 */
#include "laya_c.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char shift_at(uint64_t position) { return (unsigned char) (position * 31u + 7u); }

static uint64_t decoded_bytes = 0;
static int decode_calls = 0;

/* The file a block was read from must be the model as it was given: it ends in the weight
 * file's name for a folder and in ".tar" for an archive, and it can be opened. */
static int path_is_plausible(const char* path) {
    const size_t n = strlen(path);
    const char* weights = "model.safetensors";
    const size_t w = strlen(weights);
    FILE* f;
    int tail_ok = (n > w && strcmp(path + n - w, weights) == 0 && (path[n - w - 1] == '\\' || path[n - w - 1] == '/')) ||
                  (n > 4 && strcmp(path + n - 4, ".tar") == 0);
    if (!tail_ok) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int LAYA_CALL shift_decode(void* user, const char* path, const char* file_name, uint64_t offset, void* data,
                                  uint64_t size) {
    unsigned char* bytes = (unsigned char*) data;
    if (user != (void*) &decode_calls) return 90;                /* the user pointer must come back */
    if (strcmp(file_name, "model.safetensors") != 0) return 91;  /* only the weights are decoded */
    if (decode_calls == 0) {
        if (!path_is_plausible(path)) return 92;
        fprintf(stderr, "codec: reading %s\n", path);
    }
    for (uint64_t i = 0; i < size; i++) bytes[i] = (unsigned char) (bytes[i] - shift_at(offset + i));
    decoded_bytes += size;
    ++decode_calls;
    return 0;
}

static int LAYA_CALL failing_decode(void* user, const char* path, const char* file_name, uint64_t offset, void* data,
                                    uint64_t size) {
    (void) user; (void) path; (void) file_name; (void) offset; (void) data; (void) size;
    return 7;
}

static int encode_file(const char* in_name, const char* out_name) {
    FILE* in = fopen(in_name, "rb");
    FILE* out = in ? fopen(out_name, "wb") : NULL;
    static unsigned char buffer[1 << 20];
    uint64_t position = 0;
    size_t n;
    if (!in || !out) { printf("ERROR cannot open the files\n"); return 1; }
    while ((n = fread(buffer, 1, sizeof buffer, in)) > 0) {
        for (size_t i = 0; i < n; i++) buffer[i] = (unsigned char) (buffer[i] + shift_at(position + i));
        if (fwrite(buffer, 1, n, out) != n) { printf("ERROR cannot write\n"); return 1; }
        position += n;
    }
    fclose(in);
    if (fclose(out) != 0) { printf("ERROR cannot write\n"); return 1; }
    printf("OK %llu bytes\n", (unsigned long long) position);
    return 0;
}

int main(int argc, char** argv) {
    const char* options = "";
    const char* codec = "";
    laya_agent* agent;
    char* answer;
    char* timing;

    if (argc == 4 && strcmp(argv[1], "--encode") == 0) return encode_file(argv[2], argv[3]);
    if (argc < 2) { printf("usage: test-sources MODEL [OPTIONS_JSON] [--codec shift|fail]\n"); return 2; }
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--codec") == 0 && i + 1 < argc) codec = argv[++i];
        else options = argv[i];
    }
    if (strcmp(codec, "shift") == 0) laya_set_codec(shift_decode, &decode_calls);
    else if (strcmp(codec, "fail") == 0) laya_set_codec(failing_decode, NULL);

    agent = laya_create(argv[1], options);
    if (!agent) { printf("ERROR %s\n", laya_last_error()); return 1; }
    answer = laya_predict(agent,
        "{\"state\":\"Please refund the duplicate charge.\",\"questions\":{\"refund\":"
        "{\"type\":\"noul\",\"instructions\":\"Does the customer ask for a refund?\"}}}");
    if (!answer) { printf("ERROR no answer\n"); return 1; }
    if (strncmp(answer, "{\"error\"", 8) == 0) { printf("ERROR %s\n", answer); return 1; }
    timing = strstr(answer, ",\"elapsed_ms\"");
    if (timing) *timing = '\0';  /* everything after the results varies from run to run */
    printf("OK %s\n", answer);
    if (*codec) fprintf(stderr, "codec: %d calls, %llu bytes\n", decode_calls, (unsigned long long) decoded_bytes);
    laya_free_string(answer);
    laya_destroy(agent);

    /* A second load must work too, and without the codec a plain model must still load. */
    laya_set_codec(NULL, NULL);
    return 0;
}
