#include "ads1256.h"



/* Hardware configuration for ADC
   return value: 0 -> success, 1 -> failed */
uint8_t ADS1256_HardwareInit(uint_least8_t index)
{
    // Konfiguracja potrzebnych sygna³ów do ADS1256
    GPIO_setConfig(ADS1256nRESET, GPIO_CFG_OUTPUT | GPIO_CFG_OUT_HIGH);
    GPIO_setConfig(ADS1256nCS, GPIO_CFG_OUTPUT | GPIO_CFG_OUT_LOW);
    GPIO_setConfig(ADS1256nDRDY, GPIO_CFG_INPUT);

    // Otwarcie SPI do komunikacji z ADS1256
    SPI_Params_init(&spiParams);
    spiParams.frameFormat = SPI_POL0_PHA1;
    spiParams.bitRate = 500000;
    spi = SPI_open(index, &spiParams);
    if (spi == NULL) {
        Display_printf(display, 0, 0, "ADS1256 SPI initializing error \n");
        return 1;
    }
    else {
        Display_printf(display, 0, 0, "ADS1256 SPI initialized\n");
        transaction.txBuf = (void *) spiTxBuffer;
        transaction.rxBuf = (void *) spiRxBuffer;
    }
    return 0;

}

/* ADS1256 reset function */
void ADS1256_reset(void)
{
    GPIO_write(ADS1256nRESET, 0);
    usleep(200);
    GPIO_write(ADS1256nRESET, 1);
}

/* ADS1256 Write Command function */
void ADS1256_WriteCmd(uint8_t Cmd)
{
    spiTxBuffer[0] = Cmd;
    transaction.count = 1;
    SPI_transfer(spi, &transaction);
}

/* Write data to destination register function */
void ADS1256_WriteReg(uint8_t Reg, uint8_t data)
{
    spiTxBuffer[0] = CMD_WREG | Reg;
    spiTxBuffer[1] = 0x00;
    spiTxBuffer[2] = data;
    transaction.count = 3;
    SPI_transfer(spi, &transaction);
}

/* Read a data from the destination register */
uint8_t ADS1256_Read_data(uint8_t Reg)
{
    uint8_t temp = 0;
    spiTxBuffer[0] = CMD_RREG | Reg;
    spiTxBuffer[1] = 0x00;
    transaction.count = 2;
    SPI_transfer(spi, &transaction);
    usleep(200);
    spiTxBuffer[0] = 0xFF;
    transaction.count = 1;
    SPI_transfer(spi, &transaction);
    temp = spiRxBuffer[0];
    return temp;
}

/* ADS1256 Waiting for a busy end function */
void ADS1256_WaitDRDY(void)
{
    uint32_t i = 0;
    for(i=0;i<4000000;i++){
        if(GPIO_read(ADS1256nDRDY) == 0)
            break;
        usleep(1);
    }
    if(i >= 4000000){
       Display_printf(display, 0, 0, "Time Out ...");
    }
}

/* ADS1256 Read device ID function */
uint8_t ADS1256_ReadChipID(void)
{
    uint8_t id;
    ADS1256_WaitDRDY();
    id = ADS1256_Read_data(REG_STATUS);
    return id>>4;
}

/* ADS1256 Configure ADC gain and sampling speed function */
void ADS1256_ConfigADC(ADS1256_GAIN gain, ADS1256_DRATE drate)
{
    ADS1256_WaitDRDY();
    uint8_t buf[4] = {0,0,0,0};
    buf[0] = (0<<3) | (1<<2) | (0<<1);
    buf[1] = 0x08;
    buf[2] = (0<<5) | (0<<3) | (gain<<0);
    buf[3] = ADS1256_DRATE_E[drate];
    spiTxBuffer[0] = CMD_WREG | 0;
    spiTxBuffer[1] = 0x03;
    spiTxBuffer[2] = buf[0];
    spiTxBuffer[3] = buf[1];
    spiTxBuffer[4] = buf[2];
    spiTxBuffer[5] = buf[3];
    transaction.count = 6;
    SPI_transfer(spi, &transaction);
    usleep(1000);
}

/* ADS1256 Set the channel to be read */
void ADS1256_SetChannel(uint8_t Channel)
{
    if(Channel > 7){
        return ;
    }
    ADS1256_WriteReg(REG_MUX, (Channel<<4) | (1<<3));
}

/* ADS1256 Setting mode function */
//void ADS1256_SetMode(uint8_t Mode)
//{
//    if(Mode == 0){
//        ScanMode = 0;
//    }
//    else{
//        ScanMode = 1;
//    }
//}

/* ADS1256 Device initialization */
uint8_t ADS1256_Init(void)
{
    uint8_t chipid;
    ADS1256_reset();
    chipid = ADS1256_ReadChipID();
    if(chipid == 3){
        Display_printf(display, 0, 0, "ID Read success: %d", chipid);
    }
    else{
        Display_printf(display, 0, 0, "ID Read failed");
        return 1;
    }
    ADS1256_ConfigADC(ADS1256_GAIN_1, ADS1256_15SPS);
    return 0;
}

/* ADS1256 Read ADC data function */
uint32_t ADS1256_Read_ADC_Data(void)
{
    uint32_t read = 0;
    uint8_t buf[3] = {0,0,0};
    ADS1256_WaitDRDY();
    spiTxBuffer[0] = CMD_RDATA;
    transaction.count = 1;
    SPI_transfer(spi, &transaction);
    usleep(1);
    ADS1256_WaitDRDY();
    spiTxBuffer[0] = 0xFF;
    spiTxBuffer[1] = 0xFF;
    spiTxBuffer[2] = 0xFF;
    transaction.count = 3;
    SPI_transfer(spi, &transaction);
    buf[0] = spiRxBuffer[0];
    buf[1] = spiRxBuffer[1];
    buf[2] = spiRxBuffer[2];
    read = ((uint32_t)buf[0] << 16) & 0x00FF0000;
    read |= ((uint32_t)buf[1] << 8) & 0x0000FF00;
    read |= buf[2];
    if (read & 0x800000)
        read |= 0xFF000000;
    return read;
}

/* ADS1256 Read specified channel data */
uint32_t ADS1256_GetChannelValue(uint8_t Channel)
{
    uint32_t Value = 0;
    while(GPIO_read(ADS1256nDRDY) == 1)
    {
        usleep(1);
    }
    if(Channel>=8){
        return 0;
    }
    ADS1256_SetChannel(Channel);
    usleep(5);
    ADS1256_WriteCmd(CMD_SYNC);
    usleep(5);
    ADS1256_WriteCmd(CMD_WAKEUP);
    usleep(25);
    Value = ADS1256_Read_ADC_Data();
    return Value;
}

/* ADS1256 Read data from all channels function */
void ADS1256_GetAll(uint32_t *ADC_Value)
{
    uint8_t i;
    for(i = 0; i<8; i++){
        ADC_Value[i] = ADS1256_GetChannelValue(i);
    }
}

/* Read temperature from all channels */
void ADS1256_ReadTemperature(float *Temperature)
{
    ADS1256_GetAll(Adc_Value);
    Temperature[0] = (Adc_Value[0]*5.488e-13f*Adc_Value[0]) + (Adc_Value[0]*3.092e-5f) - 25.732;
    Temperature[1] = (Adc_Value[1]*5.445e-13f*Adc_Value[1]) + (Adc_Value[1]*3.094e-5f) - 25.354;
    Temperature[2] = (Adc_Value[2]*5.458e-13f*Adc_Value[2]) + (Adc_Value[2]*3.094e-5f) - 24.952;
    Temperature[3] = (Adc_Value[3]*5.530e-13f*Adc_Value[3]) + (Adc_Value[3]*3.086e-5f) - 25.032;
    Temperature[4] = (Adc_Value[4]*5.393e-13f*Adc_Value[4]) + (Adc_Value[4]*3.082e-5f) - 25.381;
    Temperature[5] = (Adc_Value[5]*5.414e-13f*Adc_Value[5]) + (Adc_Value[5]*3.087e-5f) - 25.062;
    Temperature[6] = (Adc_Value[6]*5.445e-13f*Adc_Value[6]) + (Adc_Value[6]*3.083e-5f) - 25.507;
    Temperature[7] = (Adc_Value[7]*5.398e-13f*Adc_Value[7]) + (Adc_Value[7]*3.087e-5f) - 25.498;
}


