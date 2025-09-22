
# BlindCash

Este projeto consiste em um dispositivo eletrônico, baseado no microcontrolador Raspberry Pi Pico, capaz de identificar cédulas de Real (BRL). O sistema utiliza um sensor de cor para ler as notas e fornece feedback sonoro e visual, tornando-o uma ferramenta de acessibilidade.

## Funcionalidades Principais

* **Identificação de Cédulas**: Reconhece as notas de R$ 2, R$ 5, R$ 10, R$ 20, R$ 50 e R$ 100.
* **Feedback Sonoro**: Anuncia o valor da nota identificada através de áudios pré-gravados.
* **Feedback Visual**:
    * Um LED RGB indica o modo de operação atual.
    * Uma matriz de LEDs 5x5 exibe a cor da nota em tempo real durante a leitura.
* **Dois Modos de Operação**:
    1.  **Modo de Inferência**: O modo padrão para identificar as notas.
    2.  **Modo de Calibração**: Permite ao usuário recalibrar as cores de referência para cada cédula, melhorando a precisão do dispositivo em diferentes condições de iluminação.

## Como Usar

1.  **Ligar o Dispositivo**: Ao ser energizado, o dispositivo inicia no **Modo de Inferência**. O LED de status ficará verde.
2.  **Identificar uma Nota**:
    * Posicione uma cédula sobre o sensor de cor.
    * Pressione o "Botão de Inferência".
    * O dispositivo emitirá 3 bipes, a matriz de LEDs acenderá com a cor da nota, e ao final da leitura o valor será anunciado.
3.  **Trocar para o Modo de Calibração**:
    * Pressione e segure o "Botão de Estado" por 2 segundos.
    * O dispositivo anunciará "Modo de Calibração" e o LED de status ficará roxo.
4.  **Calibrar uma Nota**:
    * No modo de calibração, mova o joystick para os lados para selecionar a nota que deseja calibrar (o valor será anunciado).
    * Quando a nota desejada for selecionada, pressione o "Botão de Inferência" e siga as instruções sonoras para escanear a frente e o verso da cédula.
5.  **Retornar ao Modo de Inferência**:
    * Pressione e segure o "Botão de Estado" novamente por 2 segundos.
