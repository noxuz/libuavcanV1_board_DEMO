/*
 * Copyright (c) 2020, NXP. All rights reserved.
 * Distributed under The MIT License.
 * Author: Abraham Rodriguez <abraham.rodriguez@nxp.com>
 */

/** @file
 * Source driver file for the media layer of Libuavcan v1 targeting
 * the NXP S32K14 family of automotive grade MCU's running
 * CAN-FD at 4Mbit/s data phase and 1Mbit/s in nominal phase.
 */

#include "libuavcan/driver/include/s32k_libuavcan.hpp"

namespace libuavcan
{
namespace media
{
/**
 * @namespace S32K
 * Microcontroller-specific constants, variables and non-mutating helper functions for the use of the FlexCAN peripheral
 */
namespace S32K
{
/** Number of capable CAN-FD FlexCAN instances */
constexpr static std::uint_fast8_t CANFD_Count = TARGET_S32K_CANFD_COUNT;

/** Tunable frame capacity for the ISR reception FIFO, each frame adds 80 bytes of required .bss memory */
constexpr static std::size_t Frame_Capacity = 40u;

/** Number of filters supported by a single FlexCAN instance */
constexpr static std::uint8_t Filter_Count = 5u;

/** Lookup table for NVIC IRQ numbers for each FlexCAN instance */
constexpr static std::uint32_t FlexCAN_NVIC_Indices[][2u] = {{2u, 0x20000}, {2u, 0x1000000}, {2u, 0x80000000}};

/** Array of each FlexCAN instance's addresses for dereferencing from */
constexpr static CAN_Type* FlexCAN[] = CAN_BASE_PTRS;

/** Lookup table for FlexCAN indices in PCC register */
constexpr static std::uint8_t PCC_FlexCAN_Index[] = {36u, 37u, 43u};

/** Size in words (4 bytes) of the offset between the location of message buffers in FlexCAN's dedicated RAM */
constexpr static std::uint8_t MB_Size_Words = 18u;

/** Offset in words for reaching the payload of a message buffer */
constexpr static std::uint8_t MB_Data_Offset = 2u;

/** Number of cycles to wait for the timed polls, corresponding to a timeout of 1/(80Mhz) * 2^24 = 0.2 seconds approx */
constexpr static std::uint32_t cycles_timeout = 0xFFFFFF;

/** Frame's reception FIFO as a dequeue with libuavcan's static memory pool, one for each available interface */
static std::deque<CAN::Frame<CAN::TypeFD::MaxFrameSizeBytes>,
                  platform::memory::PoolAllocator<Frame_Capacity, sizeof(CAN::Frame<CAN::TypeFD::MaxFrameSizeBytes>)>>
    g_frame_ISRbuffer[CANFD_Count];

/** Counter for the number of discarded messages due to the RX FIFO being full */
volatile static std::uint32_t g_discarded_frames_count[CANFD_Count] = {DISCARD_COUNT_ARRAY};

/**
 * Enumeration for converting from a bit number to an index, used for some registers where a bit flag for a nth
 * message buffer is represented as a bit left shifted nth times. e.g. 2nd MB is 0b100 = 4 = (1 << 2)
 */
enum MB_bit_to_index : std::uint8_t
{
    MessageBuffer0 = 0x1,  /**< Number representing the bit for the zeroth MB (1 << 2) */
    MessageBuffer1 = 0x2,  /**< Number for the bit of the first  MB (1 << 3) */
    MessageBuffer2 = 0x4,  /**< Number for the bit of the second MB (1 << 2) */
    MessageBuffer3 = 0x8,  /**< Number for the bit of the third  MB (1 << 3) */
    MessageBuffer4 = 0x10, /**< Number for the bit of the fourth MB (1 << 4) */
    MessageBuffer5 = 0x20, /**< Number for the bit of the fifth  MB (1 << 5) */
    MessageBuffer6 = 0x40, /**< Number for the bit of the sixth  MB (1 << 6) */
};

/**
 * Helper function for block polling a bit flag until it is set with a timeout of 0.2 seconds using a LPIT timer,
 * the argument list and usage reassembles the classic block polling while loop, and instead of using a third
 * argument to decide if it'ss a timed block for a clear or set, the two flavors of the function are provided.
 *
 * @param  flagRegister Register where the flag is located.
 * @param  flagMask     Mask to AND'nd with the register for isolating the flag.
 * @return libuavcan::Result::Success If the flag set before the timeout expiration..
 * @return libuavcan::Result::Failure If a timeout ocurred before the desired flag set.
 */
libuavcan::Result flagPollTimeout_Set(volatile std::uint32_t& flagRegister, std::uint32_t flag_Mask)
{
    /* Initialization of delta for timeout measurement */
    volatile std::uint32_t delta = 0;

    /* Disable LPIT channel 2 for loading */
    LPIT0->CLRTEN |= LPIT_CLRTEN_CLR_T_EN_2(1);

    /* Load LPIT with its maximum value */
    LPIT0->TMR[2].TVAL = LPIT_TMR_CVAL_TMR_CUR_VAL_MASK;

    /* Enable LPIT channel 2 for timeout start */
    LPIT0->SETTEN |= LPIT_SETTEN_SET_T_EN_2(1);

    /* Start of timed block */
    while (delta < cycles_timeout)
    {
        /* Check if the flag has been set */
        if (flagRegister & flag_Mask)
        {
            return libuavcan::Result::Success;
        }

        /* Get current value of delta */
        delta = LPIT_TMR_CVAL_TMR_CUR_VAL_MASK - (LPIT0->TMR[2].CVAL);
    }

    /* If this section is reached, means timeout ocurred and return error status is returned */
    return libuavcan::Result::Failure;
}

/**
 * Helper function for block polling a bit flag until it is cleared with a timeout of 0.2 seconds using a LPIT timer
 *
 * @param  flagRegister Register where the flag is located.
 * @param  flagMask     Mask to AND'nd with the register for isolating the flag.
 * @return libuavcan::Result::Success If the flag cleared before the timeout expiration..
 * @return libuavcan::Result::Failure If a timeout ocurred before the desired flag cleared.
 */
libuavcan::Result flagPollTimeout_Clear(volatile std::uint32_t& flagRegister, std::uint32_t flag_Mask)
{
    /* Initialization of delta for timeout measurement */
    volatile std::uint32_t delta = 0;

    /* Disable LPIT channel 2 for loading */
    LPIT0->CLRTEN |= LPIT_CLRTEN_CLR_T_EN_2(1);

    /* Load LPIT with its maximum value */
    LPIT0->TMR[2].TVAL = LPIT_TMR_CVAL_TMR_CUR_VAL_MASK;

    /* Enable LPIT channel 2 for timeout start */
    LPIT0->SETTEN |= LPIT_SETTEN_SET_T_EN_2(1);

    /* Start of timed block */
    while (delta < cycles_timeout)
    {
        /* Check if the flag has been set */
        if (!(flagRegister & flag_Mask))
        {
            return libuavcan::Result::Success;
        }

        /* Get current value of delta */
        delta = LPIT_TMR_CVAL_TMR_CUR_VAL_MASK - (LPIT0->TMR[2].CVAL);
    }

    /* If this section is reached, means timeout ocurred and return error status is returned */
    return libuavcan::Result::Failure;
}
}  // END namespace S32K

libuavcan::Result S32K_InterfaceGroup::messageBuffer_Transmit(std::uint_fast8_t iface_index,
                                                              std::uint8_t      TX_MB_index,
                                                              const FrameType&  frame)
{
    /* Get data length of the frame wished to be written */
    std::uint_fast8_t payloadLength = frame.getDataLength();

    /* Casting from uint8 to native uint32 for faster payload transfer to transmission message buffer */
    std::uint32_t* native_FrameData = reinterpret_cast<std::uint32_t*>(const_cast<std::uint8_t*>(frame.data));

    /* Fill up the payload's bytes, including the ones that don't add up to a full word e.g. 1,2,3,5,6,7 byte data
     * length payloads */
    for (std::uint8_t i = 0; i < (payloadLength >> 2) + std::min(1, (static_cast<std::uint8_t>(payloadLength) & 0x3));
         i++)
    {
        /* FlexCAN natively transmits the bytes in big-endian order, in order to transmit little-endian for UAVCAN,
         * a byte swap is required */
        REV_BYTES_32(native_FrameData[i],
                     S32K::FlexCAN[iface_index]->RAMn[TX_MB_index * S32K::MB_Size_Words + S32K::MB_Data_Offset + i]);
    }

    /* Fill up frame ID */
    S32K::FlexCAN[iface_index]->RAMn[TX_MB_index * S32K::MB_Size_Words + 1] = frame.id & CAN_WMBn_ID_ID_MASK;

    /* Fill up word 0 of frame and transmit it
     * Extended Data Length       (EDL) = 1
     * Bit Rate Switch            (BRS) = 1
     * Error State Indicator      (ESI) = 0
     * Message Buffer Code       (CODE) = 12 ( Transmit data frame )
     * Substitute Remote Request  (SRR) = 0
     * ID Extended Bit            (IDE) = 1
     * Remote Tx Request          (RTR) = 0
     * Data Length Code           (DLC) = frame.getdlc()
     * Counter Time Stamp  (TIME STAMP) = 0 ( Handled by hardware )
     */
    S32K::FlexCAN[iface_index]->RAMn[TX_MB_index * S32K::MB_Size_Words] =
        CAN_RAMn_DATA_BYTE_1(0x20) | CAN_WMBn_CS_DLC(frame.getDLC()) | CAN_RAMn_DATA_BYTE_0(0xCC);

    /* After a succesful transmission the interrupt flag of the corresponding message buffer is set, poll with
     * timeout for it */
    libuavcan::Result Status = S32K::flagPollTimeout_Set(S32K::FlexCAN[iface_index]->IFLAG1, 1 << TX_MB_index);

    /* Clear the flag previously polled (W1C register) */
    S32K::FlexCAN[iface_index]->IFLAG1 |= 1 << TX_MB_index;

    /* Return successful transmission request status */
    return Status;
}

libuavcan::time::Monotonic S32K_InterfaceGroup::resolve_Timestamp(std::uint64_t frame_timestamp, std::uint8_t instance)
{
    /* Harvest the peripheral's current timestamp, this is the 16-bit overflowing source clock */
    std::uint64_t FlexCAN_timestamp = S32K::FlexCAN[instance]->TIMER;

    /* Get an non-overflowing 64-bit timestamp, this is the target clock source */
    std::uint64_t target_source = static_cast<std::uint64_t>(
        (static_cast<std::uint64_t>(0xFFFFFFFF - LPIT0->TMR[1].CVAL) << 32) | (0xFFFFFFFF - LPIT0->TMR[0].CVAL));

    /* Compute the delta of time that occurred in the source clock */
    std::uint64_t source_delta =
        FlexCAN_timestamp > frame_timestamp ? FlexCAN_timestamp - frame_timestamp : frame_timestamp - FlexCAN_timestamp;

    /* Resolve the received frame's absolute timestamp and divide by 80 due the 80Mhz clock source
     * of both the source and target timers for converting them into the desired microseconds resolution */
    std::uint64_t resolved_timestamp_ISR = (target_source - source_delta) / 80;

    /* Instantiate the required Monotonic object from the resolved timestamp */
    return libuavcan::time::Monotonic::fromMicrosecond(resolved_timestamp_ISR);
}

void S32K_InterfaceGroup::S32K_libuavcan_ISR_handler(std::uint8_t instance)
{
    /* Perform the ISR atomically */
    DISABLE_INTERRUPTS()

    /* Initialize variable for finding which MB received */
    std::uint8_t MB_index = 0;

    /* Check which RX MB caused the interrupt (0b1111100) mask for 2nd-6th MB */
    switch (S32K::FlexCAN[instance]->IFLAG1 & 124)
    {
    case S32K::MessageBuffer2:
        MB_index = 2u; /* Case for 2nd MB */
        break;
    case S32K::MessageBuffer3:
        MB_index = 3u; /* Case for 3th MB */
        break;
    case S32K::MessageBuffer4:
        MB_index = 4u; /* Case for 4th MB */
        break;
    case S32K::MessageBuffer5:
        MB_index = 5u; /* Case for 5th MB */
        break;
    case S32K::MessageBuffer6:
        MB_index = 6u; /* Case for 6th MB */
        break;
    }

    /* Validate that the index didn't get stuck at 0, this would be invalid since MB's 0th and 1st are TX */
    if (MB_index)
    {
        /* Receive a frame only if the buffer its under its capacity */
        if (S32K::g_frame_ISRbuffer[instance].size() <= S32K::Frame_Capacity)
        {
            /* Harvest the Message buffer, read of the control and status word locks the MB */

            /* Get the raw DLC from the message buffer that received a frame */
            std::uint32_t dlc_ISR_raw =
                ((S32K::FlexCAN[instance]->RAMn[MB_index * S32K::MB_Size_Words]) & CAN_WMBn_CS_DLC_MASK) >>
                CAN_WMBn_CS_DLC_SHIFT;

            /* Create CAN::FrameDLC type variable from the raw dlc */
            CAN::FrameDLC dlc_ISR = CAN::FrameDLC(dlc_ISR_raw);

            /* Convert from dlc to data length in bytes */
            std::uint8_t payloadLength_ISR = S32K_InterfaceGroup::FrameType::dlcToLength(dlc_ISR);

            /* Get the id */
            std::uint32_t id_ISR =
                (S32K::FlexCAN[instance]->RAMn[MB_index * S32K::MB_Size_Words + 1]) & CAN_WMBn_ID_ID_MASK;

            /* Array for harvesting the received frame's payload */
            std::uint32_t data_ISR_word[(payloadLength_ISR >> 2) + std::min(1, (payloadLength_ISR & 0x3))];

            /* Perform the harvesting of the payload, leveraging from native 32-bit transfers and since the FlexCAN
             * expects the data to be in big-endian order, a byte swap is required from the little-endian
             * transmission UAVCAN requirement */
            for (std::uint8_t i = 0;
                 i < (payloadLength_ISR >> 2) + std::min(1, static_cast<std::uint8_t>(payloadLength_ISR) & 0x3);
                 i++)
            {
                REV_BYTES_32(S32K::FlexCAN[instance]->RAMn[MB_index * S32K::MB_Size_Words + S32K::MB_Data_Offset + i],
                             data_ISR_word[i]);
            }

            /* Harvest the frame's 16-bit hardware timestamp */
            std::uint64_t MB_timestamp = S32K::FlexCAN[instance]->RAMn[MB_index * S32K::MB_Size_Words] & 0xFFFF;

            /* Instantiate monotonic object form a resolved timestamp */
            time::Monotonic timestamp_ISR = resolve_Timestamp(MB_timestamp, instance);

            /* Create Frame object with constructor */
            CAN::Frame<CAN::TypeFD::MaxFrameSizeBytes> FrameISR(id_ISR,
                                                                reinterpret_cast<std::uint8_t*>(data_ISR_word),
                                                                dlc_ISR,
                                                                timestamp_ISR);

            /* Insert the frame into the queue */
            S32K::g_frame_ISRbuffer[instance].push_back(FrameISR);
        }
        else
        {
            /* Increment the number of discarded frames due to full RX dequeue */
            S32K::g_discarded_frames_count[instance]++;
        }

        /* Clear MB interrupt flag (write 1 to clear)*/
        S32K::FlexCAN[instance]->IFLAG1 |= (1 << MB_index);
    }

    /* Enable interrupts back */
    ENABLE_INTERRUPTS()
}

std::uint_fast8_t S32K_InterfaceGroup::getInterfaceCount() const
{
    return S32K::CANFD_Count;
}

libuavcan::Result S32K_InterfaceGroup::write(std::uint_fast8_t interface_index,
                                             const FrameType (&frames)[TxFramesLen],
                                             std::size_t  frames_len,
                                             std::size_t& out_frames_written)
{
    /* Initialize return value status */
    libuavcan::Result Status = libuavcan::Result::BufferFull;

    /* Input validation */
    if ((frames_len > TxFramesLen) || (interface_index > S32K::CANFD_Count))
    {
        Status = libuavcan::Result::BadArgument;
    }

    /* Poll the Inactive Message Buffer and Valid Priority Status flags before checking for free MB's */
    if ((S32K::FlexCAN[interface_index - 1]->ESR2 & CAN_ESR2_IMB_MASK) &&
        (S32K::FlexCAN[interface_index - 1]->ESR2 & CAN_ESR2_VPS_MASK))
    {
        /* Look for the lowest number free MB */
        std::uint8_t mb_index = (S32K::FlexCAN[interface_index - 1]->ESR2 & CAN_ESR2_LPTM_MASK) >> CAN_ESR2_LPTM_SHIFT;

        /* Proceed with the tranmission */
        Status = messageBuffer_Transmit(interface_index - 1, mb_index, frames[0]);

        /* Argument assignment to 1 Frame transmitted successfully */
        out_frames_written = isSuccess(Status) ? TxFramesLen : 0;
    }

    /* Return status code */
    return Status;
}

libuavcan::Result S32K_InterfaceGroup::read(std::uint_fast8_t interface_index,
                                            FrameType (&out_frames)[RxFramesLen],
                                            std::size_t& out_frames_read)
{
    /* Initialize return value and out_frames_read output reference value */
    libuavcan::Result Status = libuavcan::Result::SuccessNothing;
    out_frames_read          = 0;

    /* Input validation */
    if (interface_index > S32K::CANFD_Count)
    {
        Status = libuavcan::Result::BadArgument;
    }

    if (isSuccess(Status))
    {
        /* Check if the ISR buffer isn't empty */
        if (!S32K::g_frame_ISRbuffer[interface_index - 1].empty())
        {
            /* Get the front element of the queue buffer */
            out_frames[0] = S32K::g_frame_ISRbuffer[interface_index - 1].front();

            /* Pop the front element of the queue buffer */
            S32K::g_frame_ISRbuffer[interface_index - 1].pop_front();

            /* Default RX number of frames read at once by this implementation is 1 */
            out_frames_read = RxFramesLen;

            /* If read is successful, status is success */
            Status = libuavcan::Result::Success;
        }
    }

    /* Return status code */
    return Status;
}

libuavcan::Result S32K_InterfaceGroup::reconfigureFilters(const typename FrameType::Filter* filter_config,
                                                          std::size_t                       filter_config_length)
{
    /* Initialize return value status */
    libuavcan::Result Status = libuavcan::Result::Success;

    /* Input validation */
    if (filter_config_length > S32K::Filter_Count)
    {
        Status = libuavcan::Result::BadArgument;
    }

    if (isSuccess(Status))
    {
        for (std::uint8_t i = 0; i < S32K::CANFD_Count; i++)
        {
            /* Enter freeze mode for filter reconfiguration */
            S32K::FlexCAN[i]->MCR |= (CAN_MCR_HALT_MASK | CAN_MCR_FRZ_MASK);

            /* Block for freeze mode entry, halts any transmission or reception */
            if (isSuccess(Status))
            {
                /* Reset all previous filter configurations */
                for (std::uint8_t j = 0; j < CAN_RAMn_COUNT; j++)
                {
                    S32K::FlexCAN[i]->RAMn[j] = 0;
                }

                /* Clear the reception masks before configuring the new ones needed */
                for (std::uint8_t j = 0; j < CAN_RXIMR_COUNT; j++)
                {
                    S32K::FlexCAN[i]->RXIMR[j] = 0;
                }

                for (std::uint8_t j = 0; j < filter_config_length; j++)
                {
                    /* Setup reception MB's mask from input argument */
                    S32K::FlexCAN[i]->RXIMR[j + 2] = filter_config[j].mask;

                    /* Setup word 0 (4 Bytes) for ith MB
                     * Extended Data Length      (EDL) = 1
                     * Bit Rate Switch           (BRS) = 1
                     * Error State Indicator     (ESI) = 0
                     * Message Buffer Code      (CODE) = 4 ( Active for reception and empty )
                     * Substitute Remote Request (SRR) = 0
                     * ID Extended Bit           (IDE) = 1
                     * Remote Tx Request         (RTR) = 0
                     * Data Length Code          (DLC) = 0 ( Valid for transmission only )
                     * Counter Time Stamp (TIME STAMP) = 0 ( Handled by hardware )
                     */
                    S32K::FlexCAN[i]->RAMn[(j + 2) * S32K::MB_Size_Words] =
                        CAN_RAMn_DATA_BYTE_0(0xC4) | CAN_RAMn_DATA_BYTE_1(0x20);

                    /* Setup Message buffers 2-7 29-bit extended ID from parameter */
                    S32K::FlexCAN[i]->RAMn[(j + 2) * S32K::MB_Size_Words + 1] = filter_config[j].id;
                }

                /* Freeze mode exit request */
                S32K::FlexCAN[i]->MCR &= ~(CAN_MCR_HALT_MASK | CAN_MCR_FRZ_MASK);

                /* Block for freeze mode exit */
                if (isSuccess(Status))
                {
                    Status = S32K::flagPollTimeout_Clear(S32K::FlexCAN[i]->MCR, CAN_MCR_FRZACK_MASK);

                    /* Block until module is ready */
                    if (isSuccess(Status))
                    {
                        Status = S32K::flagPollTimeout_Clear(S32K::FlexCAN[i]->MCR, CAN_MCR_NOTRDY_MASK);
                    }
                }
            }
        }
    }

    /* Return status code */
    return Status;
}

libuavcan::Result S32K_InterfaceGroup::select(libuavcan::duration::Monotonic timeout, bool ignore_write_available)
{
    /* Obtain timeout from object */
    std::uint32_t cycles_timeout = static_cast<std::uint32_t>(timeout.toMicrosecond());

    /* Initialization of delta variable for comparison */
    volatile std::uint32_t delta = 0;

    /* Disable LPIT channel 3 for loading */
    LPIT0->CLRTEN |= LPIT_CLRTEN_CLR_T_EN_3(1);

    /* Load LPIT with its maximum value */
    LPIT0->TMR[3].TVAL = LPIT_TMR_CVAL_TMR_CUR_VAL_MASK;

    /* Enable LPIT channel 3 for timeout start */
    LPIT0->SETTEN |= LPIT_SETTEN_SET_T_EN_3(1);

    /* Start of timed block */
    while (delta < cycles_timeout)
    {
        /* Poll in each of the available interfaces */
        for (std::uint8_t i = 0; i < S32K::CANFD_Count; i++)
        {
            /* Poll for available frames in RX FIFO */
            if (!S32K::g_frame_ISRbuffer[i].empty())
            {
                return libuavcan::Result::Success;
            }

            /* Check for available message buffers for transmission if ignore_write_available is false */
            else if (!ignore_write_available)
            {
                /* Poll the Inactive Message Buffer and Valid Priority Status flags for TX availability */
                if ((S32K::FlexCAN[i]->ESR2 & CAN_ESR2_IMB_MASK) && (S32K::FlexCAN[i]->ESR2 & CAN_ESR2_VPS_MASK))
                {
                    return libuavcan::Result::Success;
                }
            }
        }

        /* Get current value of delta */
        delta = LPIT_TMR_CVAL_TMR_CUR_VAL_MASK - (LPIT0->TMR[3].CVAL);
    }

    /* If this section is reached, means timeout occurred and return timeout status */
    return libuavcan::Result::SuccessTimeout;
}

libuavcan::Result S32K_InterfaceManager::startInterfaceGroup(
    const typename InterfaceGroupType::FrameType::Filter* filter_config,
    std::size_t                                           filter_config_length,
    InterfaceGroupPtrType&                                out_group)
{
    /* Initialize return values */
    libuavcan::Result Status = libuavcan::Result::Success;
    out_group                = nullptr;

    /* Input validation */
    if (filter_config_length > S32K::Filter_Count)
    {
        Status = libuavcan::Result::BadArgument;
    }

    /* SysClock initialization for feeding 80Mhz to FlexCAN */

    /* System Oscillator (SOSC) initialization for 8Mhz external crystal */
    SCG->SOSCCSR &= ~SCG_SOSCCSR_LK_MASK;     /* Ensure the register is unlocked */
    SCG->SOSCCSR &= ~SCG_SOSCCSR_SOSCEN_MASK; /* Disable SOSC for setup */
    SCG->SOSCCFG = SCG_SOSCCFG_EREFS_MASK |   /* Setup external crystal for SOSC reference */
                   SCG_SOSCCFG_RANGE(2);      /* Select 8Mhz range */
    SCG->SOSCCSR = SCG_SOSCCSR_SOSCEN_MASK;   /* Enable SOSC reference */
    SCG->SOSCCSR |= SCG_SOSCCSR_LK_MASK;      /* Lock the register from accidental writes */

    /* Poll for valid SOSC reference, needs 4096 cycles */
    while (!(SCG->SOSCCSR & SCG_SOSCCSR_SOSCVLD_MASK))
    {
    };

    /* System PLL (SPLL) initialization for to 160Mhz reference */
    SCG->SPLLCSR &= ~SCG_SPLLCSR_LK_MASK;     /* Ensure the register is unlocked */
    SCG->SPLLCSR &= ~SCG_SPLLCSR_SPLLEN_MASK; /* Disable PLL for setup */
    SCG->SPLLCFG = SCG_SPLLCFG_MULT(24);      /* Select multiply factor of 40 for 160Mhz SPLL_CLK */
    SCG->SPLLDIV |= SCG_SPLLDIV_SPLLDIV2(1);  /* Divide by 1 for 80Mhz at SPLLDIV2 output for LPIT */
    SCG->SPLLCSR |= SCG_SPLLCSR_SPLLEN_MASK;  /* Enable PLL */
    SCG->SPLLCSR |= SCG_SPLLCSR_LK_MASK;      /* Lock register from accidental writes */

    /* Poll for valid SPLL reference */
    while (!(SCG->SPLLCSR & SCG_SPLLCSR_SPLLVLD_MASK))
    {
    };

    /* Normal RUN configuration for output clocks */
    SCG->RCCR = SCG_RCCR_SCS(6) |     /* Select SPLL as system clock source */
                SCG_RCCR_DIVCORE(1) | /* Additional dividers for Normal Run mode */
                SCG_RCCR_DIVBUS(1) | SCG_RCCR_DIVSLOW(2);

    /* CAN frames timestamping 64-bit timer initialization using chained LPIT channel 0 and 1 */

    /* Clock source option 6: (SPLLDIV2) at 80Mhz */
    PCC->PCCn[PCC_LPIT_INDEX] |= PCC_PCCn_PCS(6);
    PCC->PCCn[PCC_LPIT_INDEX] |= PCC_PCCn_CGC(1); /* Clock gating to LPIT module */

    /* Enable module */
    LPIT0->MCR |= LPIT_MCR_M_CEN(1);

    /* Select 32-bit periodic Timer for both chained channels and timeouts timer (default)  */
    LPIT0->TMR[0].TCTRL |= LPIT_TMR_TCTRL_MODE(0);
    LPIT0->TMR[1].TCTRL |= LPIT_TMR_TCTRL_MODE(0);
    LPIT0->TMR[2].TCTRL |= LPIT_TMR_TCTRL_MODE(0);

    /* Select chain mode for channel 1, this becomes the most significant 32 bits */
    LPIT0->TMR[1].TCTRL |= LPIT_TMR_TCTRL_CHAIN(1);

    /* Setup max reload value for both channels 0xFFFFFFFF */
    LPIT0->TMR[0].TVAL = LPIT_TMR_TVAL_TMR_VAL_MASK;
    LPIT0->TMR[1].TVAL = LPIT_TMR_TVAL_TMR_VAL_MASK;

    /* Start the timers */
    LPIT0->SETTEN |= LPIT_SETTEN_SET_T_EN_0(1) | LPIT_SETTEN_SET_T_EN_1(1);

    /* Verify that the least significant 32-bit timer is counting (not locked at 0xFFFFFFFF) */
    while (LPIT0->TMR[0].CVAL == LPIT_TMR_CVAL_TMR_CUR_VAL_MASK)
    {
    };

    /* FlexCAN instances initialization */
    for (std::uint8_t i = 0; i < S32K::CANFD_Count; i++)
    {
        PCC->PCCn[S32K::PCC_FlexCAN_Index[i]] = PCC_PCCn_CGC_MASK; /* FlexCAN clock gating */
        S32K::FlexCAN[i]->MCR |= CAN_MCR_MDIS_MASK;        /* Disable FlexCAN module for clock source selection */
        S32K::FlexCAN[i]->CTRL1 &= ~CAN_CTRL1_CLKSRC_MASK; /* Clear any previous clock source configuration */
        S32K::FlexCAN[i]->CTRL1 |= CAN_CTRL1_CLKSRC_MASK;  /* Select SYS_CLK as source (80Mhz)*/
        S32K::FlexCAN[i]->MCR &= ~CAN_MCR_MDIS_MASK;       /* Enable FlexCAN peripheral */
        S32K::FlexCAN[i]->MCR |= (CAN_MCR_HALT_MASK | CAN_MCR_FRZ_MASK); /* Request freeze mode etry */

        /* Block for freeze mode entry */
        while (!(S32K::FlexCAN[i]->MCR & CAN_MCR_FRZACK_MASK))
        {
        };

        /* Next configurations are only permitted in freeze mode */
        S32K::FlexCAN[i]->MCR |= CAN_MCR_FDEN_MASK |          /* Habilitate CANFD feature */
                                 CAN_MCR_FRZ_MASK;            /* Enable freeze mode entry when HALT bit is asserted */
        S32K::FlexCAN[i]->CTRL2 |= CAN_CTRL2_ISOCANFDEN_MASK; /* Activate the use of ISO 11898-1 CAN-FD standard */

        /* CAN Bit Timing (CBT) configuration for a nominal phase of 1 Mbit/s with 80 time quantas,
           in accordance with Bosch 2012 specification, sample point at 83.75% */
        S32K::FlexCAN[i]->CBT |= CAN_CBT_BTF_MASK |     /* Enable extended bit timing configurations for CAN-FD
                                                                                                                 for setting
                                                           up separetely nominal and data phase */
                                 CAN_CBT_EPRESDIV(0) |  /* Prescaler divisor factor of 1 */
                                 CAN_CBT_EPROPSEG(46) | /* Propagation segment of 47 time quantas */
                                 CAN_CBT_EPSEG1(18) |   /* Phase buffer segment 1 of 19 time quantas */
                                 CAN_CBT_EPSEG2(12) |   /* Phase buffer segment 2 of 13 time quantas */
                                 CAN_CBT_ERJW(12);      /* Resynchronization jump width same as PSEG2 */

        /* CAN-FD Bit Timing (FDCBT) for a data phase of 4 Mbit/s with 20 time quantas,
           in accordance with Bosch 2012 specification, sample point at 75% */
        S32K::FlexCAN[i]->FDCBT |=
            CAN_FDCBT_FPRESDIV(0) | /* Prescaler divisor factor of 1 */
            CAN_FDCBT_FPROPSEG(7) | /* Propagation semgment of 7 time quantas
                                                                   (only register that doesn't add 1) */
            CAN_FDCBT_FPSEG1(6) |   /* Phase buffer segment 1 of 7 time quantas */
            CAN_FDCBT_FPSEG2(4) |   /* Phase buffer segment 2 of 5 time quantas */
            CAN_FDCBT_FRJW(4);      /* Resynchorinzation jump width same as PSEG2 */

        /* Additional CAN-FD configurations */
        S32K::FlexCAN[i]->FDCTRL |= CAN_FDCTRL_FDRATE_MASK | /* Enable bit rate switch in data phase of frame */
                                    CAN_FDCTRL_TDCEN_MASK |  /* Enable transceiver delay compensation */
                                    CAN_FDCTRL_TDCOFF(5) |   /* Setup 5 cycles for data phase sampling delay */
                                    CAN_FDCTRL_MBDSR0(3);    /* Setup 64 bytes per message buffer (7 MB's) */

        /* Message buffers are located in a dedicated RAM inside FlexCAN, they aren't affected by reset,
         * so they must be explicitly initialized, they total 128 slots of 4 words each, which sum
         * to 512 bytes, each MB is 72 byte in size ( 64 payload and 8 for headers )
         */
        for (std::uint8_t j = 0; j < CAN_RAMn_COUNT; j++)
        {
            S32K::FlexCAN[i]->RAMn[j] = 0;
        }

        /* Clear the reception masks before configuring the ones needed */
        for (std::uint8_t j = 0; j < CAN_RXIMR_COUNT; j++)
        {
            S32K::FlexCAN[i]->RXIMR[j] = 0;
        }

        /* Setup maximum number of message buffers as 7, 0th and 1st for transmission and 2nd-6th for RX */
        S32K::FlexCAN[i]->MCR &= ~CAN_MCR_MAXMB_MASK; /* Clear previous configuracion of MAXMB, default is 0xF */
        S32K::FlexCAN[i]->MCR |= CAN_MCR_MAXMB(6) |
                                 CAN_MCR_SRXDIS_MASK | /* Disable self-reception of frames if ID matches */
                                 CAN_MCR_IRMQ_MASK;    /* Enable individual message buffer masking */

        /* Setup Message buffers 2nd-6th for reception and set filters */
        for (std::uint8_t j = 0; j < filter_config_length; j++)
        {
            /* Setup reception MB's mask from input argument */
            S32K::FlexCAN[i]->RXIMR[j + 2] = filter_config[j].mask;

            /* Setup word 0 (4 Bytes) for ith MB
             * Extended Data Length      (EDL) = 1
             * Bit Rate Switch           (BRS) = 1
             * Error State Indicator     (ESI) = 0
             * Message Buffer Code      (CODE) = 4 ( Active for reception and empty )
             * Substitute Remote Request (SRR) = 0
             * ID Extended Bit           (IDE) = 1
             * Remote Tx Request         (RTR) = 0
             * Data Length Code          (DLC) = 0 ( Valid for transmission only )
             * Counter Time Stamp (TIME STAMP) = 0 ( Handled by hardware )
             */
            S32K::FlexCAN[i]->RAMn[(j + 2) * S32K::MB_Size_Words] =
                CAN_RAMn_DATA_BYTE_0(0xC4) | CAN_RAMn_DATA_BYTE_1(0x20);

            /* Setup Message buffers 2-7 29-bit extended ID from parameter */
            S32K::FlexCAN[i]->RAMn[(j + 2) * S32K::MB_Size_Words + 1] = filter_config[j].id;
        }

        /* Enable interrupt in NVIC for FlexCAN reception with default priority (ID = 81) */
        S32_NVIC->ISER[S32K::FlexCAN_NVIC_Indices[i][0]] = S32K::FlexCAN_NVIC_Indices[i][1];

        /* Enable interrupts of reception MB's (0b1111100) */
        S32K::FlexCAN[i]->IMASK1 = CAN_IMASK1_BUF31TO0M(124);

        /* Exit from freeze mode */
        S32K::FlexCAN[i]->MCR &= ~(CAN_MCR_HALT_MASK | CAN_MCR_FRZ_MASK);

        /* Block for freeze mode exit */
        while (S32K::FlexCAN[i]->MCR & CAN_MCR_FRZACK_MASK)
        {
        };

        /* Block for module ready flag */
        while (S32K::FlexCAN[i]->MCR & CAN_MCR_NOTRDY_MASK)
        {
        };
    }

    /* Clock gating and multiplexing for the pins used */
    PCC->PCCn[PCC_PORTE_INDEX] |= PCC_PCCn_CGC_MASK; /* Clock gating to PORT E */
    PORTE->PCR[4] |= PORT_PCR_MUX(5);                /* CAN0_RX at PORT E pin 4 */
    PORTE->PCR[5] |= PORT_PCR_MUX(5);                /* CAN0_TX at PORT E pin 5 */

#if defined(MCU_S32K146) || defined(MCU_S32K148)

    PCC->PCCn[PCC_PORTA_INDEX] |= PCC_PCCn_CGC_MASK; /* Clock gating to PORT A */
    PORTA->PCR[12] |= PORT_PCR_MUX(3);               /* CAN1_RX at PORT A pin 12 */
    PORTA->PCR[13] |= PORT_PCR_MUX(3);               /* CAN1_TX at PORT A pin 13 */

    /* Set to LOW the standby (STB) pin in both transceivers of the UCANS32K146 node board */
    if (UAVCAN_NODE_BOARD_USED)
    {
        PORTE->PCR[11] |= PORT_PCR_MUX(1); /* MUX to GPIO */
        PTE->PDDR |= 1 << 11;              /* Set direction as output */
        PTE->PCOR |= 1 << 11;              /* Set the pin LOW */

        PORTE->PCR[10] |= PORT_PCR_MUX(1); /* Same as above */
        PTE->PDDR |= 1 << 10;
        PTE->PCOR |= 1 << 10;
    }

#endif

#if defined(MCU_S32K148)
    PCC->PCCn[PCC_PORTB_INDEX] |= PCC_PCCn_CGC_MASK; /* Clock gating to PORT B */
    PORTB->PCR[12] |= PORT_PCR_MUX(4);               /* CAN2_RX at PORT B pin 12 */
    PORTB->PCR[13] |= PORT_PCR_MUX(4);               /* CAN2_TX at PORT B pin 13 */
#endif

    /* If function ended successfully, return address of object member of type S32K_InterfaceGroup */
    out_group = &S32K_InterfaceGroupObj_;

    /* Return code for start of S32K_InterfaceGroup */
    return Status;
}

libuavcan::Result S32K_InterfaceManager::stopInterfaceGroup(InterfaceGroupPtrType& inout_group)
{
    /* Initialize return value status */
    libuavcan::Result Status = libuavcan::Result::Success;

    /* FlexCAN module deinitialization */
    for (std::uint8_t i = 0; i < S32K::CANFD_Count; i++)
    {
        /* Disable FlexCAN module */
        S32K::FlexCAN[i]->MCR |= CAN_MCR_MDIS_MASK;

        if (isSuccess(Status))
        {
            /* Poll for Low Power ACK, waits for current transmission/reception to finish */
            Status = S32K::flagPollTimeout_Set(S32K::FlexCAN[i]->MCR, CAN_MCR_LPMACK_MASK);

            if (isSuccess(Status))
            {
                /* Disable FlexCAN clock gating */
                PCC->PCCn[S32K::PCC_FlexCAN_Index[i]] &= ~PCC_PCCn_CGC_MASK;
            }
        }
    }

    /* Reset LPIT timer peripheral, (resets all except the MCR register) */
    LPIT0->MCR |= LPIT_MCR_SW_RST(1);

    /* Verify that the timer did reset (locked at 0xFFFFFFFF) */
    while (LPIT0->TMR[0].CVAL != LPIT_TMR_CVAL_TMR_CUR_VAL_MASK)
    {
    };

    /* Clear the reset bit since it isn't cleared automatically */
    LPIT0->MCR &= ~LPIT_MCR_SW_RST_MASK;

    /* Disable the clock to the LPIT's timers */
    LPIT0->MCR &= ~LPIT_MCR_M_CEN_MASK;

    /* Disable LPIT clock gating */
    PCC->PCCn[PCC_LPIT_INDEX] &= ~PCC_PCCn_CGC_MASK;

    /* Assign to null the pointer output argument */
    inout_group = nullptr;

    /* Return status code */
    return Status;
}

std::size_t S32K_InterfaceManager::getMaxFrameFilters() const
{
    return S32K::Filter_Count;
}

}  // END namespace media
}  // END namespace libuavcan

/**
 * Interrupt service routines handled by hardware in each frame reception, they are installed by the linker
 * in function of the number of instances available in the target MCU, the names match the ones from the defined
 * interrupt vector table from the startup code located in the startup_S32K14x.S file.
 */
extern "C"
{
    void CAN0_ORed_0_15_MB_IRQHandler() { libuavcan::media::S32K_InterfaceGroup::S32K_libuavcan_ISR_handler(0u); }

#if defined(MCU_S32K146) || defined(MCU_S32K148)
    void CAN1_ORed_0_15_MB_IRQHandler() { libuavcan::media::S32K_InterfaceGroup::S32K_libuavcan_ISR_handler(1u); }
#endif

#if defined(MCU_S32K148)
    void CAN2_ORed_0_15_MB_IRQHandler() { libuavcan::media::S32K_InterfaceGroup::S32K_libuavcan_ISR_handler(2u); }
#endif
}