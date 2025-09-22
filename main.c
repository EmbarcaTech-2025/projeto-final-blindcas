#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/adc.h"
#include "audios.h"
#include "hardware/pio.h"
#include "ws2818b.pio.h"

#define NUM_NOTAS 6
#define NUM_AMOSTRAS_POR_EXTREMIDADE 2
#define LIMIAR_DISTANCIA 8000
#define TEMPO_SCAN_MS 2000
#define INTERVALO_AMOSTRA_MS 10
#define BRILHO_LEITURA 0.3f

#define BOTAO_INFERENCIA_PIN 5
#define BOTAO_ESTADO_PIN 6
#define LEDR 12
#define LEDG 11
#define LEDB 13
#define BUZZER_PIN 21
#define JOYSTICK_X_PIN 27
#define JOYSTICK_X_ADC_CH 1
#define ADC_THRESHOLD 1000
#define LED_MATRIX_PIN 7

#define I2C_PORT i2c0
#define I2C_SDA 0
#define I2C_SCL 1
#define TCS34725_ADDRESS 0x29
#define COMMAND_BIT 0x80
#define ENABLE_REG 0x00
#define ATIME_REG 0x01
#define CONTROL_REG 0x0F
#define CDATAL 0x14
#define RDATAL 0x16
#define GDATAL 0x18
#define BDATAL 0x1A

#define LED_COUNT 25

typedef struct { uint16_t r, g, b; } Cor;
typedef struct {
    char nome[10];
    Cor frente_ref1; Cor frente_ref2;
    Cor verso_ref1; Cor verso_ref2;
} CedulaData;
CedulaData cedulasCalibradas[NUM_NOTAS];
typedef enum { STATE_INFERENCE, STATE_CALIBRATION } SystemState;
volatile SystemState current_state = STATE_INFERENCE;
volatile int nota_calibracao_idx = 0;
typedef struct { uint8_t G, R, B; } pixel_t;
typedef pixel_t npLED_t;
npLED_t leds_buffer[LED_COUNT];
PIO np_pio;
uint sm;

void npInit(uint pin) { uint offset = pio_add_program(pio0, &ws2818b_program); np_pio = pio0; sm = pio_claim_unused_sm(np_pio, false); if (sm < 0) { np_pio = pio1; sm = pio_claim_unused_sm(np_pio, true); } ws2818b_program_init(np_pio, sm, offset, pin, 800000.f); }
void npSetLED(const uint index, const uint8_t r, const uint8_t g, const uint8_t b) { leds_buffer[index].R = r; leds_buffer[index].G = g; leds_buffer[index].B = b; }
void npClear() { for (uint i = 0; i < LED_COUNT; ++i) npSetLED(i, 0, 0, 0); }
void npWrite() { for (uint i = 0; i < LED_COUNT; ++i) { pio_sm_put_blocking(np_pio, sm, leds_buffer[i].G); pio_sm_put_blocking(np_pio, sm, leds_buffer[i].R); pio_sm_put_blocking(np_pio, sm, leds_buffer[i].B); } }

void pwm_init_buzzer(uint pin) { gpio_set_function(pin, GPIO_FUNC_PWM); uint slice_num = pwm_gpio_to_slice_num(pin); pwm_config config = pwm_get_default_config(); pwm_config_set_clkdiv(&config, 4.f); pwm_init(slice_num, &config, true); pwm_set_gpio_level(pin, 0);}
void play_tone(uint pin, uint frequency, uint duration_ms) { uint slice_num = pwm_gpio_to_slice_num(pin); if (frequency == 0) { pwm_set_enabled(slice_num, false); sleep_ms(duration_ms); return; } pwm_config config = pwm_get_default_config(); pwm_config_set_clkdiv(&config, 4.f); pwm_init(slice_num, &config, false); uint32_t clock_freq = clock_get_hz(clk_sys); uint32_t top = (clock_freq / 4.f) / frequency - 1; pwm_set_wrap(slice_num, top); pwm_set_gpio_level(pin, top / 2); pwm_set_enabled(slice_num, true); sleep_ms(duration_ms); pwm_set_enabled(slice_num, false); }
void play_audio_segment(const uint8_t *audio, uint32_t size, uint slice_num) { const float gain_factor = 200.0f; for (uint32_t i = 0; i < size; i++) { int32_t sample = audio[i] - 128; sample *= gain_factor; sample += 2048; if (sample > 4095) sample = 4095; if (sample < 0) sample = 0; pwm_set_gpio_level(BUZZER_PIN, (uint16_t)sample); sleep_us(1000000 / 16000); } sleep_ms(100); }
void led_rgb_put(bool r, bool g, bool b) { gpio_put(LEDR, r); gpio_put(LEDG, g); gpio_put(LEDB, b); }
void tcs_write8(uint8_t reg, uint8_t value) { uint8_t buffer[2] = { COMMAND_BIT | reg, value }; i2c_write_blocking(i2c0, TCS34725_ADDRESS, buffer, 2, false); }
uint16_t tcs_read16(uint8_t reg) { uint8_t buffer[2]; uint8_t command = COMMAND_BIT | reg; i2c_write_blocking(i2c0, TCS34725_ADDRESS, &command, 1, true); i2c_read_blocking(i2c0, TCS34725_ADDRESS, buffer, 2, false); return (uint16_t)(buffer[1] << 8) | buffer[0]; }
void config_hardware() { i2c_init(i2c0, 100 * 1000); gpio_set_function(I2C_SDA, GPIO_FUNC_I2C); gpio_set_function(I2C_SCL, GPIO_FUNC_I2C); gpio_pull_up(I2C_SDA); gpio_pull_up(I2C_SCL); gpio_init(LEDR); gpio_init(LEDG); gpio_init(LEDB); gpio_set_dir(LEDR, GPIO_OUT); gpio_set_dir(LEDG, GPIO_OUT); gpio_set_dir(LEDB, GPIO_OUT); gpio_init(BOTAO_INFERENCIA_PIN); gpio_set_dir(BOTAO_INFERENCIA_PIN, GPIO_IN); gpio_pull_up(BOTAO_INFERENCIA_PIN); gpio_init(BOTAO_ESTADO_PIN); gpio_set_dir(BOTAO_ESTADO_PIN, GPIO_IN); gpio_pull_up(BOTAO_ESTADO_PIN); pwm_init_buzzer(BUZZER_PIN); adc_init(); adc_gpio_init(JOYSTICK_X_PIN); }
void tcs_init() { tcs_write8(ENABLE_REG, 0x00); tcs_write8(ATIME_REG, 0xEB); tcs_write8(CONTROL_REG, 0x01); tcs_write8(ENABLE_REG, 0x03); sleep_ms(200); }

Cor escanear_face() {
    uint32_t r_total = 0, g_total = 0, b_total = 0;
    int contagem = 0;
    uint32_t start_time = time_us_32();
    while (time_us_32() - start_time < (TEMPO_SCAN_MS * 1000)) {
        uint16_t clear = tcs_read16(CDATAL);
        if (clear == 0) clear = 1;
        uint8_t r_atual = ((uint32_t)tcs_read16(RDATAL) * 255) / clear;
        uint8_t g_atual = ((uint32_t)tcs_read16(GDATAL) * 255) / clear;
        uint8_t b_atual = ((uint32_t)tcs_read16(BDATAL) * 255) / clear;
        uint8_t r_display = (uint8_t)((float)r_atual * BRILHO_LEITURA);
        uint8_t g_display = (uint8_t)((float)g_atual * BRILHO_LEITURA);
        uint8_t b_display = (uint8_t)((float)b_atual * BRILHO_LEITURA);
        for (int i = 0; i < LED_COUNT; i++) {
            npSetLED(i, r_display, g_display, b_display);
        }
        npWrite();
        r_total += r_atual;
        g_total += g_atual;
        b_total += b_atual;
        contagem++;
        sleep_ms(INTERVALO_AMOSTRA_MS);
    }
    npClear();
    npWrite();
    if (contagem == 0) contagem = 1;
    Cor cor_media = {(uint16_t)(r_total / contagem), (uint16_t)(g_total / contagem), (uint16_t)(b_total / contagem)};
    return cor_media;
}

void emitir_bipes(int contagem) { for (int i = 0; i < contagem; i++) { play_tone(BUZZER_PIN, 650, 300); sleep_ms(100); } }
const char* identificar_cedula(Cor cor_lida) { long menor_distancia = -1; int indice_melhor_candidato = -1; for (int i = 0; i < NUM_NOTAS; i++) { Cor refs[4] = { cedulasCalibradas[i].frente_ref1, cedulasCalibradas[i].frente_ref2, cedulasCalibradas[i].verso_ref1, cedulasCalibradas[i].verso_ref2 }; for (int j = 0; j < 4; j++) { long dr = cor_lida.r - refs[j].r; long dg = cor_lida.g - refs[j].g; long db = cor_lida.b - refs[j].b; long dist = dr*dr + dg*dg + db*db; if (menor_distancia == -1 || dist < menor_distancia) { menor_distancia = dist; indice_melhor_candidato = i; } } } if (menor_distancia > LIMIAR_DISTANCIA) { return "Desconhecido"; } else { return cedulasCalibradas[indice_melhor_candidato].nome; } }
void anunciar_nota_inferencia(const char* resultado) { uint slice_num = pwm_gpio_to_slice_num(BUZZER_PIN); pwm_set_wrap(slice_num, (1 << 12) - 1); pwm_set_clkdiv(slice_num, 1.0f); pwm_set_enabled(slice_num, true); if (strcmp(resultado, "R$ 2") == 0) { play_audio_segment(audio_dois, audio_dois_size, slice_num); play_audio_segment(audio_reais, audio_reais_size, slice_num); } else if (strcmp(resultado, "R$ 5") == 0) { play_audio_segment(audio_cinco, audio_cinco_size, slice_num); play_audio_segment(audio_reais, audio_reais_size, slice_num); } else if (strcmp(resultado, "R$ 10") == 0) { play_audio_segment(audio_dez, audio_dez_size, slice_num); play_audio_segment(audio_reais, audio_reais_size, slice_num); } else if (strcmp(resultado, "R$ 20") == 0) { play_audio_segment(audio_vinte, audio_vinte_size, slice_num); play_audio_segment(audio_reais, audio_reais_size, slice_num); } else if (strcmp(resultado, "R$ 50") == 0) { play_audio_segment(audio_cinquenta, audio_cinquenta_size, slice_num); play_audio_segment(audio_reais, audio_reais_size, slice_num); } else if (strcmp(resultado, "R$ 100") == 0) { play_audio_segment(audio_cem, audio_cem_size, slice_num); play_audio_segment(audio_reais, audio_reais_size, slice_num); } else { play_tone(BUZZER_PIN, 440, 500); } pwm_set_enabled(slice_num, false); }
void carregar_medias_padrao() { strcpy(cedulasCalibradas[0].nome, "R$ 2"); cedulasCalibradas[0].frente_ref1 = (Cor){63, 88, 83}; cedulasCalibradas[0].frente_ref2 = (Cor){64, 88, 82}; cedulasCalibradas[0].verso_ref1 = (Cor){66, 90, 78}; cedulasCalibradas[0].verso_ref2 = (Cor){64, 89, 81}; strcpy(cedulasCalibradas[1].nome, "R$ 5"); cedulasCalibradas[1].frente_ref1 = (Cor){72, 84, 79}; cedulasCalibradas[1].frente_ref2 = (Cor){70, 85, 79}; cedulasCalibradas[1].verso_ref1 = (Cor){72, 82, 81}; cedulasCalibradas[1].verso_ref2 = (Cor){72, 83, 80}; strcpy(cedulasCalibradas[2].nome, "R$ 10"); cedulasCalibradas[2].frente_ref1 = (Cor){80, 84, 71}; cedulasCalibradas[2].frente_ref2 = (Cor){80, 83, 72}; cedulasCalibradas[2].verso_ref1 = (Cor){83, 84, 69}; cedulasCalibradas[2].verso_ref2 = (Cor){82, 84, 69}; strcpy(cedulasCalibradas[3].nome, "R$ 20"); cedulasCalibradas[3].frente_ref1 = (Cor){82, 91, 62}; cedulasCalibradas[3].frente_ref2 = (Cor){86, 89, 59}; cedulasCalibradas[3].verso_ref1 = (Cor){87, 88, 59}; cedulasCalibradas[3].verso_ref2 = (Cor){93, 85, 56}; strcpy(cedulasCalibradas[4].nome, "R$ 50"); cedulasCalibradas[4].frente_ref1 = (Cor){84, 87, 63}; cedulasCalibradas[4].frente_ref2 = (Cor){73, 89, 73}; cedulasCalibradas[4].verso_ref1 = (Cor){79, 89, 66}; cedulasCalibradas[4].verso_ref2 = (Cor){80, 88, 67}; strcpy(cedulasCalibradas[5].nome, "R$ 100"); cedulasCalibradas[5].frente_ref1 = (Cor){58, 92, 84}; cedulasCalibradas[5].frente_ref2 = (Cor){58, 91, 84}; cedulasCalibradas[5].verso_ref1 = (Cor){57, 93, 84}; cedulasCalibradas[5].verso_ref2 = (Cor){62, 90, 82}; }
void anunciar_nota_calibracao(const char* nota, const char* lado) { uint slice_num = pwm_gpio_to_slice_num(BUZZER_PIN); pwm_set_wrap(slice_num, (1 << 12) - 1); pwm_set_clkdiv(slice_num, 1.0f); pwm_set_enabled(slice_num, true); if (strcmp(nota, "R$ 2") == 0) { play_audio_segment(audio_dois, audio_dois_size, slice_num); } else if (strcmp(nota, "R$ 5") == 0) { play_audio_segment(audio_cinco, audio_cinco_size, slice_num); } else if (strcmp(nota, "R$ 10") == 0) { play_audio_segment(audio_dez, audio_dez_size, slice_num); } else if (strcmp(nota, "R$ 20") == 0) { play_audio_segment(audio_vinte, audio_vinte_size, slice_num); } else if (strcmp(nota, "R$ 50") == 0) { play_audio_segment(audio_cinquenta, audio_cinquenta_size, slice_num); } else if (strcmp(nota, "R$ 100") == 0) { play_audio_segment(audio_cem, audio_cem_size, slice_num); } play_audio_segment(audio_reais, audio_reais_size, slice_num); if (strcmp(lado, "frente") == 0) { play_audio_segment(audio_frente, audio_frente_size, slice_num); } else if (strcmp(lado, "verso") == 0) { play_audio_segment(audio_verso, audio_verso_size, slice_num); } pwm_set_enabled(slice_num, false); }
void anunciar_calibracao_concluida(const char* nota) { uint slice_num = pwm_gpio_to_slice_num(BUZZER_PIN); pwm_set_wrap(slice_num, (1 << 12) - 1); pwm_set_clkdiv(slice_num, 1.0f); pwm_set_enabled(slice_num, true); if (strcmp(nota, "R$ 2") == 0) { play_audio_segment(audio_dois, audio_dois_size, slice_num); } else if (strcmp(nota, "R$ 5") == 0) { play_audio_segment(audio_cinco, audio_cinco_size, slice_num); } else if (strcmp(nota, "R$ 10") == 0) { play_audio_segment(audio_dez, audio_dez_size, slice_num); } else if (strcmp(nota, "R$ 20") == 0) { play_audio_segment(audio_vinte, audio_vinte_size, slice_num); } else if (strcmp(nota, "R$ 50") == 0) { play_audio_segment(audio_cinquenta, audio_cinquenta_size, slice_num); } else if (strcmp(nota, "R$ 100") == 0) { play_audio_segment(audio_cem, audio_cem_size, slice_num); } play_audio_segment(audio_reais, audio_reais_size, slice_num); play_audio_segment(audio_calibracao, audio_calibracao_size, slice_num); play_audio_segment(audio_concluida, audio_concluida_size, slice_num); pwm_set_enabled(slice_num, false); }
void calibrar_nota_selecionada(int index) { const char* nome_nota = cedulasCalibradas[index].nome; uint32_t r_total_f1 = 0, g_total_f1 = 0, b_total_f1 = 0; for (int j = 0; j < NUM_AMOSTRAS_POR_EXTREMIDADE; j++) { anunciar_nota_calibracao(nome_nota, "frente"); emitir_bipes(j + 1); while(gpio_get(BOTAO_INFERENCIA_PIN) == 1) { sleep_ms(10); } while(gpio_get(BOTAO_INFERENCIA_PIN) == 0) { sleep_ms(10); } play_tone(BUZZER_PIN, 650, 300); Cor amostra = escanear_face(); r_total_f1 += amostra.r; g_total_f1 += amostra.g; b_total_f1 += amostra.b; } cedulasCalibradas[index].frente_ref1 = (Cor){r_total_f1 / NUM_AMOSTRAS_POR_EXTREMIDADE, g_total_f1 / NUM_AMOSTRAS_POR_EXTREMIDADE, b_total_f1 / NUM_AMOSTRAS_POR_EXTREMIDADE}; uint32_t r_total_f2 = 0, g_total_f2 = 0, b_total_f2 = 0; for (int j = 0; j < NUM_AMOSTRAS_POR_EXTREMIDADE; j++) { anunciar_nota_calibracao(nome_nota, "frente"); emitir_bipes(j + 1); while(gpio_get(BOTAO_INFERENCIA_PIN) == 1) { sleep_ms(10); } while(gpio_get(BOTAO_INFERENCIA_PIN) == 0) { sleep_ms(10); } play_tone(BUZZER_PIN, 650, 300); Cor amostra = escanear_face(); r_total_f2 += amostra.r; g_total_f2 += amostra.g; b_total_f2 += amostra.b; } cedulasCalibradas[index].frente_ref2 = (Cor){r_total_f2 / NUM_AMOSTRAS_POR_EXTREMIDADE, g_total_f2 / NUM_AMOSTRAS_POR_EXTREMIDADE, b_total_f2 / NUM_AMOSTRAS_POR_EXTREMIDADE}; uint32_t r_total_v1 = 0, g_total_v1 = 0, b_total_v1 = 0; for (int j = 0; j < NUM_AMOSTRAS_POR_EXTREMIDADE; j++) { anunciar_nota_calibracao(nome_nota, "verso"); emitir_bipes(j + 1); while(gpio_get(BOTAO_INFERENCIA_PIN) == 1) { sleep_ms(10); } while(gpio_get(BOTAO_INFERENCIA_PIN) == 0) { sleep_ms(10); } play_tone(BUZZER_PIN, 650, 300); Cor amostra = escanear_face(); r_total_v1 += amostra.r; g_total_v1 += amostra.g; b_total_v1 += amostra.b; } cedulasCalibradas[index].verso_ref1 = (Cor){r_total_v1 / NUM_AMOSTRAS_POR_EXTREMIDADE, g_total_v1 / NUM_AMOSTRAS_POR_EXTREMIDADE, b_total_v1 / NUM_AMOSTRAS_POR_EXTREMIDADE}; uint32_t r_total_v2 = 0, g_total_v2 = 0, b_total_v2 = 0; for (int j = 0; j < NUM_AMOSTRAS_POR_EXTREMIDADE; j++) { anunciar_nota_calibracao(nome_nota, "verso"); emitir_bipes(j + 1); while(gpio_get(BOTAO_INFERENCIA_PIN) == 1) { sleep_ms(10); } while(gpio_get(BOTAO_INFERENCIA_PIN) == 0) { sleep_ms(10); } play_tone(BUZZER_PIN, 650, 300); Cor amostra = escanear_face(); r_total_v2 += amostra.r; g_total_v2 += amostra.g; b_total_v2 += amostra.b; } cedulasCalibradas[index].verso_ref2 = (Cor){r_total_v2 / NUM_AMOSTRAS_POR_EXTREMIDADE, g_total_v2 / NUM_AMOSTRAS_POR_EXTREMIDADE, b_total_v2 / NUM_AMOSTRAS_POR_EXTREMIDADE}; anunciar_calibracao_concluida(nome_nota); }

int main() {
    stdio_init_all();
    config_hardware();
    npInit(LED_MATRIX_PIN);
    npClear();
    npWrite();
    sleep_ms(4000);
    tcs_init();
    carregar_medias_padrao();
    printf("\n--- MODO DE INFERENCIA ATIVO ---\n");
    bool joystick_moved = false;
    while (true) {
        uint slice_num = pwm_gpio_to_slice_num(BUZZER_PIN);
        if (gpio_get(BOTAO_ESTADO_PIN) == 0) {
            uint32_t press_start = time_us_32();
            sleep_ms(50);
            while (gpio_get(BOTAO_ESTADO_PIN) == 0) { sleep_ms(10); }
            uint32_t press_duration = (time_us_32() - press_start) / 1000;
            if (press_duration >= 2000) { current_state = (current_state == STATE_INFERENCE) ? STATE_CALIBRATION : STATE_INFERENCE; pwm_set_wrap(slice_num, (1 << 12) - 1); pwm_set_clkdiv(slice_num, 1.0f); pwm_set_enabled(slice_num, true); if (current_state == STATE_CALIBRATION) { printf("\n--- MODO DE CALIBRACAO ATIVO ---\n"); play_audio_segment(audio_calibracao, audio_calibracao_size, slice_num); } else { printf("\n--- MODO DE INFERENCIA ATIVO ---\n"); play_audio_segment(audio_inferencia, audio_inferencia_size, slice_num); } pwm_set_enabled(slice_num, false);
            } else { pwm_set_wrap(slice_num, (1 << 12) - 1); pwm_set_clkdiv(slice_num, 1.0f); pwm_set_enabled(slice_num, true); if (current_state == STATE_INFERENCE) { play_audio_segment(audio_inferencia, audio_inferencia_size, slice_num); } else { play_audio_segment(audio_calibracao, audio_calibracao_size, slice_num); } pwm_set_enabled(slice_num, false); }
        }
        if (current_state == STATE_INFERENCE) {
            led_rgb_put(false, true, false);
            if (gpio_get(BOTAO_INFERENCIA_PIN) == 0) {
                sleep_ms(50);
                if (gpio_get(BOTAO_INFERENCIA_PIN) == 0) {
                    printf("Preparando para ler...\n"); sleep_ms(1000); emitir_bipes(3);
                    printf("Identificando...\n");
                    Cor cor_lida = escanear_face();
                    const char* resultado = identificar_cedula(cor_lida);
                    printf("--> Nota: %s (R:%d G:%d B:%d)\n", resultado, cor_lida.r, cor_lida.g, cor_lida.b);
                    anunciar_nota_inferencia(resultado);
                }
                while (gpio_get(BOTAO_INFERENCIA_PIN) == 0) { sleep_ms(10); }
            }
        } else {
            led_rgb_put(true, false, true);
            adc_select_input(JOYSTICK_X_ADC_CH);
            uint16_t adc_x = adc_read();
            if (adc_x < ADC_THRESHOLD && !joystick_moved) { nota_calibracao_idx = (nota_calibracao_idx - 1 + NUM_NOTAS) % NUM_NOTAS; printf("Nota selecionada: %s\n", cedulasCalibradas[nota_calibracao_idx].nome); anunciar_nota_inferencia(cedulasCalibradas[nota_calibracao_idx].nome); joystick_moved = true;
            } else if (adc_x > (4095 - ADC_THRESHOLD) && !joystick_moved) { nota_calibracao_idx = (nota_calibracao_idx + 1) % NUM_NOTAS; printf("Nota selecionada: %s\n", cedulasCalibradas[nota_calibracao_idx].nome); anunciar_nota_inferencia(cedulasCalibradas[nota_calibracao_idx].nome); joystick_moved = true;
            } else if (adc_x >= ADC_THRESHOLD && adc_x <= (4095 - ADC_THRESHOLD)) { joystick_moved = false; }
            if (gpio_get(BOTAO_INFERENCIA_PIN) == 0) {
                sleep_ms(50);
                if (gpio_get(BOTAO_INFERENCIA_PIN) == 0) {
                    calibrar_nota_selecionada(nota_calibracao_idx);
                    printf("Nota %s recalibrada. Volte para Inferencia.\n", cedulasCalibradas[nota_calibracao_idx].nome);
                }
                while (gpio_get(BOTAO_INFERENCIA_PIN) == 0) { sleep_ms(10); }
            }
        }
        sleep_ms(20);
    }
    return 0;
}