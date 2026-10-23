// Test de host para el codec: cc -I components/adpcm/include test/host/test_adpcm.c components/adpcm/adpcm.c -lm
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "adpcm.h"

#define N 8000

int main(void)
{
    static int16_t pcm[N], out[N];
    static uint8_t enc[N / 2];

    for (int i = 0; i < N; i++) {
        pcm[i] = (int16_t)(8000 * sinf(2 * M_PI * 440 * i / 8000.0f) + 3000 * sinf(2 * M_PI * 1250 * i / 8000.0f));
    }

    adpcm_state_t st = {0};
    adpcm_encode(&st, pcm, N, enc);
    st = (adpcm_state_t){0};
    adpcm_decode(&st, enc, N, out);

    double sig = 0, err = 0;
    for (int i = 0; i < N; i++) {
        sig += (double)pcm[i] * pcm[i];
        err += (double)(pcm[i] - out[i]) * (pcm[i] - out[i]);
    }
    const double snr = 10 * log10(sig / err);
    printf("SNR %.1f dB\n", snr);
    if (snr < 20) {
        printf("FAIL: SNR too low\n");
        return 1;
    }
    printf("OK\n");
    return 0;
}
