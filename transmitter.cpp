/*
    fm_transmitter - use Raspberry Pi as FM transmitter

    Copyright (c) 2019, Marcin Kondej
    All rights reserved.

    See https://github.com/markondej/fm_transmitter

    Redistribution and use in source and binary forms, with or without modification, are
    permitted provided that the following conditions are met:

    1. Redistributions of source code must retain the above copyright notice, this list
    of conditions and the following disclaimer.

    2. Redistributions in binary form must reproduce the above copyright notice, this
    list of conditions and the following disclaimer in the documentation and/or other
    materials provided with the distribution.

    3. Neither the name of the copyright holder nor the names of its contributors may be
    used to endorse or promote products derived from this software without specific
    prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
    EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
    OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
    SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
    INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
    TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
    BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
    WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "transmitter.hpp"
#include "mailbox.h"
#include <bcm_host.h>
#include <thread>
#include <cmath>
#include <fcntl.h>
#include <sys/mman.h>

#define PERIPHERALS_PHYS_BASE 0x7E000000
#define BCM2835_PERI_VIRT_BASE 0x20000000
#define BCM2838_PERI_VIRT_BASE 0xFE000000
#define DMA0_BASE_OFFSET 0x00007000
#define DMA15_BASE_OFFSET 0x00E05000
#define CLK0_BASE_OFFSET 0x00101070
#define PWMCLK_BASE_OFFSET 0x001010A0
#define GPIO_BASE_OFFSET 0x00200000
#define PWM_BASE_OFFSET 0x0020C000
#define TIMER_BASE_OFFSET 0x00003000

#define BCM2835_MEM_FLAG 0x0C
#define BCM2838_MEM_FLAG 0x04

#define BCM2835_PLLD_FREQ 500
#define BCM2838_PLLD_FREQ 750

#define BUFFER_TIME 1000000
#define PWM_WRITES_PER_SAMPLE 10
#define PWM_CHANNEL_RANGE 32
#define PAGE_SIZE 4096

struct TimerRegisters {
    uint32_t ctlStatus;
    uint32_t low;
    uint32_t high;
    uint32_t c0;
    uint32_t c1;
    uint32_t c2;
    uint32_t c3;
};

struct ClockRegisters {
    uint32_t ctl;
    uint32_t div;
};

struct PWMRegisters {
    uint32_t ctl;
    uint32_t status;
    uint32_t dmaConf;
    uint32_t reserved0;
    uint32_t chn1Range;
    uint32_t chn1Data;
    uint32_t fifoIn;
    uint32_t reserved1;
    uint32_t chn2Range;
    uint32_t chn2Data;
};

struct DMAControllBlock {
    uint32_t transferInfo;
    uint32_t srcAddress;
    uint32_t dstAddress;
    uint32_t transferLen;
    uint32_t stride;
    uint32_t nextCbAddress;
    uint32_t reserved0;
    uint32_t reserved1;
};

struct DMARegisters {
    uint32_t ctlStatus;
    uint32_t cbAddress;
    uint32_t transferInfo;
    uint32_t srcAddress;
    uint32_t dstAddress;
    uint32_t transferLen;
    uint32_t stride;
    uint32_t nextCbAddress;
    uint32_t debug;
};

struct AllocatedMemory {
    uint32_t handle, size, physicalBase, virtualBase;
    int mBoxFd;
};

void *Transmitter::peripherals = nullptr;
bool Transmitter::transmitting = false;
uint32_t Transmitter::sampleOffset = 0;
uint32_t Transmitter::clockDivisor = 0;
uint32_t Transmitter::divisorRange = 0;
uint32_t Transmitter::sampleRate = 0;
volatile ClockRegisters *Transmitter::output = nullptr;
std::vector<Sample> *Transmitter::buffer = nullptr;

Transmitter::Transmitter()
{
    int memFd;
    if ((memFd = open("/dev/mem", O_RDWR | O_SYNC)) < 0) {
        throw std::runtime_error("Cannot open /dev/mem (permission denied)");
    }

    peripherals = mmap(nullptr, getPeripheralsSize(), PROT_READ | PROT_WRITE, MAP_SHARED, memFd, getPeripheralsVirtBaseAddress());
    close(memFd);
    if (peripherals == MAP_FAILED) {
        throw std::runtime_error("Cannot obtain access to peripherals (mmap error)");
    }
}

Transmitter::~Transmitter()
{
    munmap(peripherals, getPeripheralsSize());
}

Transmitter &Transmitter::getInstance()
{
    static Transmitter instance;
    return instance;
}

uint32_t Transmitter::getPeripheralsVirtBaseAddress()
{
    return (bcm_host_get_peripheral_size() == BCM2838_PERI_VIRT_BASE) ? BCM2838_PERI_VIRT_BASE : bcm_host_get_peripheral_address();
}

uint32_t Transmitter::getPeripheralsSize()
{
    uint32_t size = bcm_host_get_peripheral_size();
    if (size == BCM2838_PERI_VIRT_BASE) {
        size = 0x01000000;
    }
    return size;
}

double Transmitter::getSourceFreq()
{
    return (getPeripheralsVirtBaseAddress() == BCM2838_PERI_VIRT_BASE) ? BCM2838_PLLD_FREQ : BCM2835_PLLD_FREQ;
}

uint32_t Transmitter::getPeripheralPhysAddress(volatile void *object) {
    return PERIPHERALS_PHYS_BASE + (reinterpret_cast<uint32_t>(object) - reinterpret_cast<uint32_t>(peripherals));
}

uint32_t Transmitter::getPeripheralVirtAddress(uint32_t offset) {
    return reinterpret_cast<uint32_t>(peripherals) + offset;
}

uint32_t Transmitter::getMemoryPhysAddress(AllocatedMemory &memory, volatile void *object)
{
    return memory.physicalBase + (reinterpret_cast<uint32_t>(object) - memory.virtualBase);
}

AllocatedMemory Transmitter::allocateMemory(uint32_t size)
{
    AllocatedMemory memory;
    memory.size = 0x00000000;
    memory.mBoxFd = mbox_open();
    if (size % PAGE_SIZE) {
        size = (size / PAGE_SIZE + 1) * PAGE_SIZE;
    }
    memory.handle = mem_alloc(memory.mBoxFd, size, PAGE_SIZE, (getPeripheralsVirtBaseAddress() == BCM2835_PERI_VIRT_BASE) ? BCM2835_MEM_FLAG : BCM2838_MEM_FLAG);
    if (!memory.handle) {
        mbox_close(memory.mBoxFd);
        return memory;
    }
    memory.physicalBase = mem_lock(memory.mBoxFd, memory.handle);
    memory.virtualBase = reinterpret_cast<uint32_t>(mapmem(memory.physicalBase & ~0xC0000000, size));
    memory.size = size;
    return memory;
}

void Transmitter::freeMemory(AllocatedMemory &memory)
{
    unmapmem(reinterpret_cast<void *>(memory.virtualBase), memory.size);
    mem_unlock(memory.mBoxFd, memory.handle);
    mem_free(memory.mBoxFd, memory.handle);
    mbox_close(memory.mBoxFd);
    memory.size = 0x00000000;
}

volatile PWMRegisters *Transmitter::initPwmController()
{
    volatile ClockRegisters *pwmClk = reinterpret_cast<ClockRegisters *>(getPeripheralVirtAddress(PWMCLK_BASE_OFFSET));
    double pwmClkFreq = PWM_WRITES_PER_SAMPLE * PWM_CHANNEL_RANGE * sampleRate / 1000000;
    pwmClk->ctl = (0x5A << 24) | 0x06;
    usleep(1000);
    pwmClk->div = (0x5A << 24) | static_cast<uint32_t>(getSourceFreq() * (0x01 << 12) / pwmClkFreq);
    pwmClk->ctl = (0x5A << 24) | (0x01 << 4) | 0x06;

    volatile PWMRegisters *pwm = reinterpret_cast<PWMRegisters *>(getPeripheralVirtAddress(PWM_BASE_OFFSET));
    pwm->ctl = 0x00000000;
    usleep(1000);
    pwm->status = 0x01FC;
    pwm->ctl = (0x01 << 6);
    usleep(1000);
    pwm->chn1Range = PWM_CHANNEL_RANGE;
    pwm->dmaConf = (0x01 << 31) | 0x0707;
    pwm->ctl = (0x01 << 5) | (0x01 << 2) | 0x01;
    return pwm;
}

void Transmitter::closePwmController(volatile PWMRegisters *pwm)
{
    pwm->ctl = 0x00000000;
}

volatile DMARegisters *Transmitter::startDma(AllocatedMemory &memory, volatile DMAControllBlock *dmaCb, uint8_t dmaChannel)
{
    volatile DMARegisters *dma = reinterpret_cast<DMARegisters *>(getPeripheralVirtAddress((dmaChannel < 15) ? DMA0_BASE_OFFSET + dmaChannel * 0x100 : DMA15_BASE_OFFSET));
    dma->ctlStatus = (0x01 << 31);
    usleep(1000);
    dma->ctlStatus = (0x01 << 2) | (0x01 << 1);
    dma->cbAddress = getMemoryPhysAddress(memory, dmaCb);
    dma->ctlStatus = (0xFF << 16) | 0x01;
    return dma;
}

void Transmitter::closeDma(volatile DMARegisters *dma)
{
    dma->ctlStatus = (0x01 << 31);
}

volatile ClockRegisters *Transmitter::initClockOutput()
{
    volatile ClockRegisters *clock = reinterpret_cast<ClockRegisters *>(getPeripheralVirtAddress(CLK0_BASE_OFFSET));
    volatile uint32_t *gpio = reinterpret_cast<uint32_t *>(getPeripheralVirtAddress(GPIO_BASE_OFFSET));
    clock->ctl = (0x5A << 24) | 0x06;
    usleep(1000);
    clock->div = (0x5A << 24) | clockDivisor;
    clock->ctl = (0x5A << 24) | (0x01 << 9) | (0x01 << 4) | 0x06;
    *gpio = (*gpio & 0xFFFF8FFF) | (0x01 << 14);
    return clock;
}

void Transmitter::closeClockOutput(volatile ClockRegisters *clock)
{
    clock->ctl = (0x5A << 24) | 0x06;
}

void Transmitter::transmit(WaveReader &reader, double frequency, double bandwidth, uint8_t dmaChannel, bool preserveCarrierOnExit)
{
    if (transmitting) {
        throw std::runtime_error("Cannot play, transmitter already in use");
    }

    transmitting = true;

    PCMWaveHeader header = reader.getHeader();
    uint32_t bufferSize = static_cast<uint32_t>(static_cast<uint64_t>(header.sampleRate) * BUFFER_TIME / 1000000);

    preserveCarrier = preserveCarrierOnExit;
    clockDivisor = static_cast<uint32_t>(round(getSourceFreq() * (0x01 << 12) / frequency));
    divisorRange = clockDivisor - static_cast<uint32_t>(round(getSourceFreq() * (0x01 << 12) / (frequency + 0.0005 * bandwidth)));
    sampleRate = header.sampleRate;

    if (!clockInitialized) {
        output = initClockOutput();
        clockInitialized = true;
    }

    try {
        if (dmaChannel != 0xFF) {
            transmitViaDma(reader, bufferSize, dmaChannel);
        } else {
            transmitViaCpu(reader, bufferSize);
        }
    } catch (std::runtime_error &catched) {
        closeClockOutput(output);
        throw catched;
    }

    if (!preserveCarrier) {
        closeClockOutput(output);
    }
}

void Transmitter::transmitViaCpu(WaveReader &reader, uint32_t bufferSize)
{
    std::vector<Sample> *samples = reader.getSamples(bufferSize, transmitting);
    if (samples == nullptr) {
        return;
    }

    sampleOffset = 0;
    buffer = samples;

    bool eof = samples->size() < bufferSize, errorCatched = false;
    std::thread txThread(Transmitter::transmitThread);
    std::string errorMessage;

    usleep(BUFFER_TIME / 2);

    try {
        while (!eof && transmitting) {
            if (buffer == nullptr) {
                if (!reader.setSampleOffset(sampleOffset + bufferSize)) {
                    break;
                }
                samples = reader.getSamples(bufferSize, transmitting);
                if (samples == nullptr) {
                    break;
                }
                eof = samples->size() < bufferSize;
                buffer = samples;
            }
            usleep(BUFFER_TIME / 2);
        }
    } catch (std::runtime_error &catched) {
        errorMessage = std::string(catched.what());
        errorCatched = true;
    }
    transmitting = false;
    txThread.join();
    if (buffer != nullptr) {
        delete buffer;
    }
    if (errorCatched) {
        throw std::runtime_error(errorMessage);
    }
}

void Transmitter::transmitViaDma(WaveReader &reader, uint32_t bufferSize, uint8_t dmaChannel)
{
    if (dmaChannel > 15) {
        throw std::runtime_error("DMA channel number out of range (0 - 15)");
    }

    std::vector<Sample> *samples = reader.getSamples(bufferSize, transmitting);
    if (samples == nullptr) {
        return;
    }
    bool eof = false;
    if (samples->size() < bufferSize) {
        bufferSize = samples->size();
        eof = true;
    }

    AllocatedMemory dmaMemory = allocateMemory(sizeof(uint32_t) * (bufferSize + 1) + sizeof(DMAControllBlock) * (2 * bufferSize));
    if (!dmaMemory.size) {
        delete samples;
        throw std::runtime_error("Cannot allocate memory");
    }

    volatile PWMRegisters *pwm = initPwmController();

    double value;
    uint32_t i, cbOffset = 0;
#ifndef NO_PREEMP
    PreEmphasis preEmphasis(sampleRate);
#endif

    volatile DMAControllBlock *dmaCb = reinterpret_cast<DMAControllBlock *>(dmaMemory.virtualBase);
    volatile uint32_t *clkDiv = reinterpret_cast<uint32_t *>(reinterpret_cast<uint32_t>(dmaCb) + 2 * sizeof(DMAControllBlock) * bufferSize);
    volatile uint32_t *pwmFifoData = reinterpret_cast<uint32_t *>(reinterpret_cast<uint32_t>(clkDiv) + sizeof(uint32_t) * bufferSize);
    for (i = 0; i < bufferSize; i++) {
        value = (*samples)[i].getMonoValue();
#ifndef NO_PREEMP
        value = preEmphasis.filter(value);
#endif
        clkDiv[i] = (0x5A << 24) | (clockDivisor - static_cast<int32_t>(round(value * divisorRange)));
        dmaCb[cbOffset].transferInfo = (0x01 << 26) | (0x01 << 3);
        dmaCb[cbOffset].srcAddress = getMemoryPhysAddress(dmaMemory, &clkDiv[i]);
        dmaCb[cbOffset].dstAddress = getPeripheralPhysAddress(&output->div);
        dmaCb[cbOffset].transferLen = sizeof(uint32_t);
        dmaCb[cbOffset].stride = 0;
        dmaCb[cbOffset].nextCbAddress = getMemoryPhysAddress(dmaMemory, &dmaCb[cbOffset + 1]);
        cbOffset++;

        dmaCb[cbOffset].transferInfo = (0x01 << 26) | (0x05 << 16) | (0x01 << 6) | (0x01 << 3);
        dmaCb[cbOffset].srcAddress = getMemoryPhysAddress(dmaMemory, pwmFifoData);
        dmaCb[cbOffset].dstAddress = getPeripheralPhysAddress(&pwm->fifoIn);
        dmaCb[cbOffset].transferLen = sizeof(uint32_t) * PWM_WRITES_PER_SAMPLE;
        dmaCb[cbOffset].stride = 0;
        dmaCb[cbOffset].nextCbAddress = getMemoryPhysAddress(dmaMemory, (i < bufferSize - 1) ? &dmaCb[cbOffset + 1] : dmaCb);
        cbOffset++;
    }
    *pwmFifoData = 0x00000000;
    delete samples;

    volatile DMARegisters *dma = startDma(dmaMemory, dmaCb, dmaChannel);
    bool errorCatched = false;
    std::string errorMessage;
    usleep(BUFFER_TIME / 4);

    try {
        while (!eof && transmitting) {
            samples = reader.getSamples(bufferSize, transmitting);
            if (samples == nullptr) {
                break;
            }
            cbOffset = 0;
            eof = samples->size() < bufferSize;
            for (i = 0; i < samples->size(); i++) {
                value = (*samples)[i].getMonoValue();
#ifndef NO_PREEMP
                value = preEmphasis.filter(value);
#endif
                while (i == ((dma->cbAddress - getMemoryPhysAddress(dmaMemory, dmaCb)) / (2 * sizeof(DMAControllBlock)))) {
                    usleep(1000);
                }
                clkDiv[i] = (0x5A << 24) | (clockDivisor - static_cast<int32_t>(round(value * divisorRange)));
                cbOffset += 2;
            }
            delete samples;
        }
    } catch (std::runtime_error &catched) {
        errorMessage = std::string(catched.what());
        errorCatched = true;
    }

    cbOffset = (cbOffset < 2 * bufferSize) ? cbOffset : 0;
    dmaCb[cbOffset].nextCbAddress = 0x00000000;
    while (dma->cbAddress != 0x00000000) {
        usleep(1000);
    }

    closeDma(dma);
    closePwmController(pwm);

    freeMemory(dmaMemory);
    transmitting = false;

    if (errorCatched) {
        throw std::runtime_error(errorMessage);
    }
}

void Transmitter::transmitThread()
{
    uint32_t offset, length, prevOffset;
    std::vector<Sample> *samples = nullptr;
    uint64_t start;
    double value;

#ifndef NO_PREEMP
    PreEmphasis preEmphasis(sampleRate);
#endif

    volatile TimerRegisters *timer = reinterpret_cast<TimerRegisters *>(getPeripheralVirtAddress(TIMER_BASE_OFFSET));
    uint64_t current = *(reinterpret_cast<volatile uint64_t *>(&timer->low));
    uint64_t playbackStart = current;

    while (transmitting) {
        start = current;

        while ((buffer == nullptr) && transmitting) {
            usleep(1);
            current = *(reinterpret_cast<volatile uint64_t *>(&timer->low));
        }
        if (!transmitting) {
            break;
        }

        samples = buffer;
        length = samples->size();
        buffer = nullptr;

        sampleOffset = (current - playbackStart) * sampleRate / 1000000;
        offset = (current - start) * sampleRate / 1000000;

        while (true) {
            if (offset >= length) {
                break;
            }
            prevOffset = offset;
            value = (*samples)[offset].getMonoValue();
#ifndef NO_PREEMP
            value = preEmphasis.filter(value);
#endif
            output->div = (0x5A << 24) | (clockDivisor - static_cast<int32_t>(round(value * divisorRange)));
            while (offset == prevOffset) {
                usleep(1); // asm("nop");
                current = *(reinterpret_cast<volatile uint64_t *>(&timer->low));;
                offset = (current - start) * sampleRate / 1000000;
            }
        }
        delete samples;
    }
}

void Transmitter::stop()
{
    preserveCarrier = false;
    transmitting = false;
}
