/*
 * Copyright (c) 2013-2014 The Linux Foundation. All rights reserved.
 *
 * Previously licensed under the ISC license by Qualcomm Atheros, Inc.
 *
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This file was originally distributed by Qualcomm Atheros, Inc.
 * under proprietary terms before Copyright ownership was assigned
 * to the Linux Foundation.
 */

#define ATH_MODULE_NAME hif
#include <adf_os_types.h>
#include <adf_os_dma.h>
#include <adf_os_timer.h>
#include <adf_os_time.h>
#include <adf_os_lock.h>
#include <adf_os_io.h>
#include <adf_os_mem.h>
#include <adf_os_module.h>
#include <adf_os_util.h>
#include <adf_os_stdtypes.h>
#include <adf_os_defer.h>
#include <adf_os_atomic.h>
#include <adf_nbuf.h>
#include <athdefs.h>
#include <adf_net_types.h>
#include <a_types.h>
#include <athdefs.h>
#include <a_osapi.h>
#include <hif.h>
#include <htc_services.h>
#include "hif_sdio_internal.h"
#include "regtable.h"
#include "if_ath_sdio.h"


static void HIFDevDumpRegisters(HIF_SDIO_DEVICE *pDev,
        MBOX_IRQ_PROC_REGISTERS *pIrqProcRegs,
        MBOX_IRQ_ENABLE_REGISTERS *pIrqEnableRegs)
{

    AR_DEBUG_PRINTF(ATH_DEBUG_ANY, ("RegTable->"));

    if (pIrqProcRegs != NULL) {
        AR_DEBUG_PRINTF( ATH_DEBUG_ANY,
                ("HostIntStatus: 0x%x ",pIrqProcRegs->host_int_status));
        AR_DEBUG_PRINTF( ATH_DEBUG_ANY,
                ("CPUIntStatus: 0x%x ",pIrqProcRegs->cpu_int_status));
        AR_DEBUG_PRINTF( ATH_DEBUG_ANY,
                ("ErrorIntStatus: 0x%x ",pIrqProcRegs->error_int_status));
        AR_DEBUG_PRINTF( ATH_DEBUG_ANY,
                ("CounterIntStatus: 0x%x ",pIrqProcRegs->counter_int_status));
        AR_DEBUG_PRINTF(ATH_DEBUG_ANY,
                ("MboxFrame: 0x%x ",pIrqProcRegs->mbox_frame));

        AR_DEBUG_PRINTF(ATH_DEBUG_ANY, ("\nRegTable->"));

        AR_DEBUG_PRINTF( ATH_DEBUG_ANY,
                ("RxLKAValid: 0x%x ",pIrqProcRegs->rx_lookahead_valid));
        AR_DEBUG_PRINTF( ATH_DEBUG_ANY,
                ("RxLKA0: 0x%x",pIrqProcRegs->rx_lookahead[0]));
        AR_DEBUG_PRINTF( ATH_DEBUG_ANY,
                ("RxLKA1: 0x%x ",pIrqProcRegs->rx_lookahead[1]));

        AR_DEBUG_PRINTF( ATH_DEBUG_ANY,
                ("RxLKA2: 0x%x ",pIrqProcRegs->rx_lookahead[2]));
        AR_DEBUG_PRINTF( ATH_DEBUG_ANY,
                ("RxLKA3: 0x%x",pIrqProcRegs->rx_lookahead[3]));
        AR_DEBUG_PRINTF(ATH_DEBUG_ANY, ("\nRegTable->"));

        if (pDev->MailBoxInfo.GMboxAddress != 0) {
            /* if the target supports GMBOX hardware, dump some additional state */
            AR_DEBUG_PRINTF( ATH_DEBUG_ANY,
                    ("GMBOX-HostIntStatus2:  0x%x ",pIrqProcRegs->host_int_status2));
            AR_DEBUG_PRINTF( ATH_DEBUG_ANY,
                    ("GMBOX-RX-Avail: 0x%x ",pIrqProcRegs->gmbox_rx_avail));
        }

    }

    if (pIrqEnableRegs != NULL) {
        AR_DEBUG_PRINTF( ATH_DEBUG_ANY,
                ("IntStatusEnable: 0x%x ",pIrqEnableRegs->int_status_enable));
        AR_DEBUG_PRINTF( ATH_DEBUG_ANY,
                ("CounterIntStatusEnable: 0x%x ",pIrqEnableRegs->counter_int_status_enable));
    }AR_DEBUG_PRINTF(ATH_DEBUG_ANY, ("\n"));
}

static A_STATUS HIFDevAllocAndPrepareRxPackets(HIF_SDIO_DEVICE *pDev,
        A_UINT32 LookAheads[], int Messages, HTC_PACKET_QUEUE *pQueue)
{
    A_STATUS status = A_OK;
    HTC_PACKET *pPacket;
    HTC_FRAME_HDR *pHdr;
    int i, j;
    int numMessages;
    int fullLength;
    A_BOOL noRecycle;

    /* lock RX while we assemble the packet buffers */
    LOCK_HIF_DEV_RX(pDev);

    for (i = 0; i < Messages; i++) {

        pHdr = (HTC_FRAME_HDR *) &LookAheads[MAILBOX_LOOKAHEAD_SIZE_IN_WORD*i];

        if (pHdr->EndpointID >= ENDPOINT_MAX) {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                    ("Invalid Endpoint in look-ahead: %d \n",pHdr->EndpointID));
            /* invalid endpoint */
            status = A_EPROTO;
            break;
        }

        if (pHdr->PayloadLen > HTC_MAX_PAYLOAD_LENGTH) {
            AR_DEBUG_PRINTF( ATH_DEBUG_ERR,
                    ("Payload length %d exceeds max HTC : %d !\n", pHdr->PayloadLen, (A_UINT32)HTC_MAX_PAYLOAD_LENGTH));
            status = A_EPROTO;
            break;
        }

        if ((pHdr->Flags & HTC_FLAGS_RECV_BUNDLE_CNT_MASK) == 0) {
            /* HTC header only indicates 1 message to fetch */
            numMessages = 1;
        } else {
            /* HTC header indicates that every packet to follow has the same padded length so that it can
             * be optimally fetched as a full bundle */
            numMessages = (pHdr->Flags & HTC_FLAGS_RECV_BUNDLE_CNT_MASK)
                    >> HTC_FLAGS_RECV_BUNDLE_CNT_SHIFT;
            /* the count doesn't include the starter frame, just a count of frames to follow */
            numMessages++;
            //A_ASSERT(numMessages <= target->MaxMsgPerBundle);
            AR_DEBUG_PRINTF( ATH_DEBUG_RECV,
                    ("HTC header indicates :%d messages can be fetched as a bundle \n",numMessages));
        }

        fullLength =
                DEV_CALC_RECV_PADDED_LEN(pDev,pHdr->PayloadLen + sizeof(HTC_FRAME_HDR));

        /* get packet buffers for each message, if there was a bundle detected in the header,
         * use pHdr as a template to fetch all packets in the bundle */
        for (j = 0; j < numMessages; j++) {

            /* reset flag, any packets allocated using the RecvAlloc() API cannot be recycled on cleanup,
             * they must be explicitly returned */
            noRecycle = FALSE;
            pPacket = HIFDevAllocRxBuffer(pDev);

            if (pPacket == NULL) {
                /* this is not an error, we simply need to mark that we are waiting for buffers.*/
                pDev->RecvStateFlags |= HTC_RECV_WAIT_BUFFERS;
                //pDev->EpWaitingForBuffers = pEndpoint->Id;
                status = A_NO_RESOURCE;
                break;
            }

            //AR_DEBUG_ASSERT(pPacket->Endpoint == pEndpoint->Id);
            /* clear flags */
            pPacket->PktInfo.AsRx.HTCRxFlags = 0;
            pPacket->PktInfo.AsRx.IndicationFlags = 0;
            pPacket->Status = A_OK;

            if (noRecycle) {
                /* flag that these packets cannot be recycled, they have to be returned to the
                 * user */
                pPacket->PktInfo.AsRx.HTCRxFlags |= HTC_RX_PKT_NO_RECYCLE;
            }
            /* add packet to queue (also incase we need to cleanup down below)  */
            HTC_PACKET_ENQUEUE(pQueue, pPacket);

            /*
             if (HTC_STOPPING(target)) {
             status = A_ECANCELED;
             break;
             }
             */

            /* make sure this message can fit in the endpoint buffer */
            if ((A_UINT32) fullLength > pPacket->BufferLength) {
                AR_DEBUG_PRINTF( ATH_DEBUG_ERR,
                        ("Payload Length Error : header reports payload of: %d (%d) endpoint buffer size: %d \n", pHdr->PayloadLen, fullLength, pPacket->BufferLength));
                status = A_EPROTO;
                break;
            }

            if (j > 0) {
                /* for messages fetched in a bundle the expected lookahead is unknown since we
                 * are only using the lookahead of the first packet as a template of what to
                 * expect for lengths */
                /* flag that once we get the real HTC header we need to refesh the information */
                pPacket->PktInfo.AsRx.HTCRxFlags |= HTC_RX_PKT_REFRESH_HDR;
                /* set it to something invalid */
                pPacket->PktInfo.AsRx.ExpectedHdr = 0xFFFFFFFF;
            } else {

                pPacket->PktInfo.AsRx.ExpectedHdr = LookAheads[MAILBOX_LOOKAHEAD_SIZE_IN_WORD*i]; /* set expected look ahead */
            }
            /* set the amount of data to fetch */
            pPacket->ActualLength = pHdr->PayloadLen + HTC_HDR_LENGTH;
        }

        if (A_FAILED(status)) {
            if (A_NO_RESOURCE == status) {
                /* this is actually okay */
                status = A_OK;
            }
            break;
        }

    }

    UNLOCK_HIF_DEV_RX(pDev);

    if (A_FAILED(status)) {
        while (!HTC_QUEUE_EMPTY(pQueue)) {
            pPacket = HTC_PACKET_DEQUEUE(pQueue);
            /* recycle all allocated packets */
            //HTC_RECYCLE_RX_PKT(target, pPacket,
            //        &target->EndPoint[pPacket->Endpoint]);
        }
    }

    return status;
}

#if 0
static A_STATUS HIFDevIssueRecvPacketBundle(HIF_SDIO_DEVICE *pDev,
        HTC_PACKET_QUEUE *pRecvPktQueue,
        HTC_PACKET_QUEUE *pSyncCompletionQueue,
        int *pNumPacketsFetched,
        A_BOOL PartialBundle)
{
    A_STATUS status = A_OK;
    HIF_SCATTER_REQ *pScatterReq;
    int i, totalLength;
    int pktsToScatter;
    HTC_PACKET *pPacket;
    A_BOOL asyncMode = (pSyncCompletionQueue == NULL) ? TRUE : FALSE;
    int scatterSpaceRemaining = 1; //DEV_GET_MAX_BUNDLE_RECV_LENGTH(pDev);

    pktsToScatter = HTC_PACKET_QUEUE_DEPTH(pRecvPktQueue);
    //pktsToScatter = min(pktsToScatter, target->MaxMsgPerBundle);
    pktsToScatter = min(pktsToScatter, 1);

    if ((HTC_PACKET_QUEUE_DEPTH(pRecvPktQueue) - pktsToScatter) > 0) {
        /* we were forced to split this bundle receive operation
         * all packets in this partial bundle must have their lookaheads ignored */
        PartialBundle = TRUE;
        /* this would only happen if the target ignored our max bundle limit */
        AR_DEBUG_PRINTF(ATH_DEBUG_WARN,
                ("HTCIssueRecvPacketBundle : partial bundle detected num:%d , %d \n",
                        HTC_PACKET_QUEUE_DEPTH(pRecvPktQueue), pktsToScatter));
    }

    totalLength = 0;

    AR_DEBUG_PRINTF(ATH_DEBUG_RECV,("+HTCIssueRecvPacketBundle (Numpackets: %d , actual : %d) \n",
                    HTC_PACKET_QUEUE_DEPTH(pRecvPktQueue), pktsToScatter));

    do {

        pScatterReq = DEV_ALLOC_SCATTER_REQ(&target->Device);

        if (pScatterReq == NULL) {
            /* no scatter resources left, just let caller handle it the legacy way */
            break;
        }

        pScatterReq->CallerFlags = 0;

        if (PartialBundle) {
            /* mark that this is a partial bundle, this has special ramifications to the
             * scatter completion routine */
            pScatterReq->CallerFlags |= HTC_SCATTER_REQ_FLAGS_PARTIAL_BUNDLE;
        }

        /* convert HTC packets to scatter list */
        for (i = 0; i < pktsToScatter; i++) {
            int paddedLength;

            pPacket = HTC_PACKET_DEQUEUE(pRecvPktQueue);
            A_ASSERT(pPacket != NULL);

            paddedLength = DEV_CALC_RECV_PADDED_LEN(&target->Device, pPacket->ActualLength);

            if ((scatterSpaceRemaining - paddedLength) < 0) {
                /* exceeds what we can transfer, put the packet back */
                HTC_PACKET_ENQUEUE_TO_HEAD(pRecvPktQueue,pPacket);
                break;
            }

            scatterSpaceRemaining -= paddedLength;

            if (PartialBundle || (i < (pktsToScatter - 1))) {
                /* packet 0..n-1 cannot be checked for look-aheads since we are fetching a bundle
                 * the last packet however can have it's lookahead used */
                pPacket->PktInfo.AsRx.HTCRxFlags |= HTC_RX_PKT_IGNORE_LOOKAHEAD;
            }

            /* note: 1 HTC packet per scatter entry */
            /* setup packet into */
            pScatterReq->ScatterList[i].pBuffer = pPacket->pBuffer;
            pScatterReq->ScatterList[i].Length = paddedLength;

            pPacket->PktInfo.AsRx.HTCRxFlags |= HTC_RX_PKT_PART_OF_BUNDLE;

            if (asyncMode) {
                /* save HTC packet for async completion routine */
                pScatterReq->ScatterList[i].pCallerContexts[0] = pPacket;
            } else {
                /* queue to caller's sync completion queue, caller will unload this when we return */
                HTC_PACKET_ENQUEUE(pSyncCompletionQueue,pPacket);
            }

            A_ASSERT(pScatterReq->ScatterList[i].Length);
            totalLength += pScatterReq->ScatterList[i].Length;
        }

        pScatterReq->TotalLength = totalLength;
        pScatterReq->ValidScatterEntries = i;

        if (asyncMode) {
            pScatterReq->CompletionRoutine = HTCAsyncRecvScatterCompletion;
            pScatterReq->Context = target;
        }

        status = DevSubmitScatterRequest(&target->Device, pScatterReq, DEV_SCATTER_READ, asyncMode);

        if (A_SUCCESS(status)) {
            *pNumPacketsFetched = i;
        }

        if (!asyncMode) {
            /* free scatter request */
            DEV_FREE_SCATTER_REQ(&target->Device, pScatterReq);
        }

    }while (FALSE);

    AR_DEBUG_PRINTF(ATH_DEBUG_RECV,("-HTCIssueRecvPacketBundle (status:%d) (fetched:%d) \n",
                    status,*pNumPacketsFetched));

    return status;
}
#endif

static INLINE A_STATUS HIFDevRecvPacket(HIF_SDIO_DEVICE *pDev,
        HTC_PACKET *pPacket, A_UINT32 RecvLength)
{
    A_UINT32 paddedLength;
    A_STATUS status;
    A_BOOL sync = (pPacket->Completion == NULL) ? TRUE : FALSE;
    A_UINT8 mboxIndex;

    /* adjust the length to be a multiple of block size if appropriate */
    paddedLength = DEV_CALC_RECV_PADDED_LEN(pDev, RecvLength);

    if (paddedLength > pPacket->BufferLength) {
        AR_DEBUG_PRINTF( ATH_DEBUG_ERR,
                ("DevRecvPacket, Not enough space for padlen:%d recvlen:%d bufferlen:%d \n", paddedLength,RecvLength,pPacket->BufferLength));
        if (pPacket->Completion != NULL) {
            COMPLETE_HTC_PACKET(pPacket, A_EINVAL);
            return A_OK;
        }
        return A_EINVAL;
    }

    /* mailbox index is saved in Endpoint member */
    mboxIndex = (int) pPacket->Endpoint;
    AR_DEBUG_PRINTF( ATH_DEBUG_RECV,
            ("HIFDevRecvPacket (0x%lX : hdr:0x%X) Padded Length: %d Mbox:0x%X (mode:%s)\n", (unsigned long)pPacket, pPacket->PktInfo.AsRx.ExpectedHdr, paddedLength, pDev->MailBoxInfo.MboxAddresses[mboxIndex], sync ? "SYNC" : "ASYNC"));

    status = HIFReadWrite(pDev->HIFDevice,
            pDev->MailBoxInfo.MboxAddresses[mboxIndex],
            pPacket->pBuffer,
            paddedLength,
            (sync ? HIF_RD_SYNC_BLOCK_FIX : HIF_RD_ASYNC_BLOCK_FIX),
            sync ? NULL : pPacket); /* pass the packet as the context to the HIF request */
    if (status != A_OK) {
        AR_DEBUG_PRINTF( ATH_DEBUG_RECV,
                ("HIFDevRecvPacket (0x%lX : hdr:0x%X) Failed\n", (unsigned long)pPacket, pPacket->PktInfo.AsRx.ExpectedHdr));
    }
    if (sync) {
        pPacket->Status = status;
        if (status == A_OK) {
            HTC_FRAME_HDR *pHdr = (HTC_FRAME_HDR *) pPacket->pBuffer;
            AR_DEBUG_PRINTF(ATH_DEBUG_RECV,
                    ("HIFDevRecvPacket "
                    "EP:%d,PayloadLen:%d,Flag:%d,CB:0x%02X,0x%02X\n",
                    pHdr->EndpointID,
                    pHdr->PayloadLen,
                    pHdr->Flags,
                    pHdr->ControlBytes0,
                    pHdr->ControlBytes1));
        }
    }

    return status;
}

static INLINE A_STATUS HIFDevProcessTrailer(HIF_SDIO_DEVICE *pDev,
        A_UINT8 *pBuffer, int Length, A_UINT32 *pNextLookAheads,
        int *pNumLookAheads, HTC_ENDPOINT_ID FromEndpoint)
{
    HTC_RECORD_HDR *pRecord;
    A_UINT8 *pRecordBuf;
    HTC_LOOKAHEAD_REPORT *pLookAhead;
    A_UINT8 *pOrigBuffer;
    int origLength;
    A_STATUS status;

    AR_DEBUG_PRINTF(ATH_DEBUG_RECV,
            ("+HTCProcessTrailer (length:%d) \n", Length));

    if (AR_DEBUG_LVL_CHECK(ATH_DEBUG_RECV)) {
        AR_DEBUG_PRINTBUF(pBuffer,Length,"Recv Trailer");
    }

    pOrigBuffer = pBuffer;
    origLength = Length;
    status = A_OK;

    while (Length > 0) {

        if (Length < sizeof(HTC_RECORD_HDR)) {
            status = A_EPROTO;
            break;
        }
        /* these are byte aligned structs */
        pRecord = (HTC_RECORD_HDR *) pBuffer;
        Length -= sizeof(HTC_RECORD_HDR);
        pBuffer += sizeof(HTC_RECORD_HDR);

        if (pRecord->Length > Length) {
            /* no room left in buffer for record */
            AR_DEBUG_PRINTF( ATH_DEBUG_ERR,
                    (" invalid record length: %d (id:%d) buffer has: %d bytes left \n", pRecord->Length, pRecord->RecordID, Length));
            status = A_EPROTO;
            break;
        }
        /* start of record follows the header */
        pRecordBuf = pBuffer;

        switch (pRecord->RecordID) {
        case HTC_RECORD_CREDITS:
            AR_DEBUG_ASSERT(pRecord->Length >= sizeof(HTC_CREDIT_REPORT));
            /*
             HTCProcessCreditRpt(target,
             (HTC_CREDIT_REPORT *)pRecordBuf,
             pRecord->Length / (sizeof(HTC_CREDIT_REPORT)),
             FromEndpoint);
             */
            break;
        case HTC_RECORD_LOOKAHEAD:
            AR_DEBUG_ASSERT(pRecord->Length >= sizeof(HTC_LOOKAHEAD_REPORT));
            pLookAhead = (HTC_LOOKAHEAD_REPORT *) pRecordBuf;
            if ((pLookAhead->PreValid == ((~pLookAhead->PostValid) & 0xFF))
                    && (pNextLookAheads != NULL)) {

                AR_DEBUG_PRINTF( ATH_DEBUG_RECV,
                        (" LookAhead Report Found (pre valid:0x%X, post valid:0x%X) \n", pLookAhead->PreValid, pLookAhead->PostValid));

                /* look ahead bytes are valid, copy them over */
                ((A_UINT8 *) (&pNextLookAheads[0]))[0] = pLookAhead->LookAhead0;
                ((A_UINT8 *) (&pNextLookAheads[0]))[1] = pLookAhead->LookAhead1;
                ((A_UINT8 *) (&pNextLookAheads[0]))[2] = pLookAhead->LookAhead2;
                ((A_UINT8 *) (&pNextLookAheads[0]))[3] = pLookAhead->LookAhead3;

                if (AR_DEBUG_LVL_CHECK(ATH_DEBUG_RECV)) {
                    DebugDumpBytes((A_UINT8 *) pNextLookAheads,
                            4,
                            "Next Look Ahead");
                }
                /* just one normal lookahead */
                if (pNumLookAheads != NULL) {
                    *pNumLookAheads = 1;
                }
            }
            break;
        case HTC_RECORD_LOOKAHEAD_BUNDLE:
            AR_DEBUG_ASSERT(pRecord->Length >= sizeof(HTC_BUNDLED_LOOKAHEAD_REPORT));
            if (pRecord->Length >= sizeof(HTC_BUNDLED_LOOKAHEAD_REPORT)
                    && (pNextLookAheads != NULL)) {
                HTC_BUNDLED_LOOKAHEAD_REPORT *pBundledLookAheadRpt;
                int i;

                pBundledLookAheadRpt =
                        (HTC_BUNDLED_LOOKAHEAD_REPORT *) pRecordBuf;

                if (AR_DEBUG_LVL_CHECK(ATH_DEBUG_RECV)) {
                    DebugDumpBytes(pRecordBuf,
                            pRecord->Length,
                            "Bundle LookAhead");
                }

                if ((pRecord->Length / (sizeof(HTC_BUNDLED_LOOKAHEAD_REPORT)))
                        > HTC_HOST_MAX_MSG_PER_BUNDLE) {
                    /* this should never happen, the target restricts the number
                     * of messages per bundle configured by the host */
                    A_ASSERT(FALSE);
                    status = A_EPROTO;
                    break;
                }

                for (i = 0;
                        i
                                < (int) (pRecord->Length
                                        / (sizeof(HTC_BUNDLED_LOOKAHEAD_REPORT)));
                        i++) {
                    ((A_UINT8 *) (&pNextLookAheads[i]))[0] =
                            pBundledLookAheadRpt->LookAhead0;
                    ((A_UINT8 *) (&pNextLookAheads[i]))[1] =
                            pBundledLookAheadRpt->LookAhead1;
                    ((A_UINT8 *) (&pNextLookAheads[i]))[2] =
                            pBundledLookAheadRpt->LookAhead2;
                    ((A_UINT8 *) (&pNextLookAheads[i]))[3] =
                            pBundledLookAheadRpt->LookAhead3;
                    pBundledLookAheadRpt++;
                }

                *pNumLookAheads = i;
            }
            break;
        default:
            AR_DEBUG_PRINTF( ATH_DEBUG_ERR,
                    (" unhandled record: id:%d length:%d \n", pRecord->RecordID, pRecord->Length));
            break;
        }

        if (A_FAILED(status)) {
            break;
        }

        /* advance buffer past this record for next time around */
        pBuffer += pRecord->Length;
        Length -= pRecord->Length;
    }

    if (A_FAILED(status)) {
        DebugDumpBytes(pOrigBuffer, origLength, "BAD Recv Trailer");
    }

    AR_DEBUG_PRINTF(ATH_DEBUG_RECV, ("-HTCProcessTrailer \n"));
    return status;

}

#if 0
/* process a received message (i.e. strip off header, process any trailer data)
 * note : locks must be released when this function is called */
static A_STATUS HIFDevProcessRecvHeader(HIF_SDIO_DEVICE *pDev,
        HTC_PACKET *pPacket, A_UINT32 *pNextLookAheads, int *pNumLookAheads)
{
    A_UINT8 temp;
    A_UINT8 *pBuf;
    A_STATUS status = A_OK;
    A_UINT16 payloadLen;
    A_UINT32 lookAhead, ActualLength;

    pBuf = pPacket->pBuffer;
    ActualLength = pPacket->ActualLength;

    if (pNumLookAheads != NULL) {
        *pNumLookAheads = 0;
    }

    AR_DEBUG_PRINTF(ATH_DEBUG_RECV, ("+HTCProcessRecvHeader \n"));

    if (AR_DEBUG_LVL_CHECK(ATH_DEBUG_RECV)) {
        AR_DEBUG_PRINTBUF(pBuf,pPacket->ActualLength,"HTC Recv PKT");
    }

    do {
        /* note, we cannot assume the alignment of pBuffer, so we use the safe macros to
         * retrieve 16 bit fields */
        payloadLen = HTC_GET_FIELD(pBuf, HTC_FRAME_HDR, PAYLOADLEN);

        ((A_UINT8 *) &lookAhead)[0] = pBuf[0];
        ((A_UINT8 *) &lookAhead)[1] = pBuf[1];
        ((A_UINT8 *) &lookAhead)[2] = pBuf[2];
        ((A_UINT8 *) &lookAhead)[3] = pBuf[3];

        if (pPacket->PktInfo.AsRx.HTCRxFlags & HTC_RX_PKT_REFRESH_HDR) {
            /* refresh expected hdr, since this was unknown at the time we grabbed the packets
             * as part of a bundle */
            pPacket->PktInfo.AsRx.ExpectedHdr = lookAhead;
            /* refresh actual length since we now have the real header */
            pPacket->ActualLength = payloadLen + HTC_HDR_LENGTH;

            /* validate the actual header that was refreshed  */
            if (pPacket->ActualLength > pPacket->BufferLength) {
                AR_DEBUG_PRINTF( ATH_DEBUG_ERR,
                        ("Refreshed HDR payload length (%d) in bundled RECV is invalid (hdr: 0x%X) \n", payloadLen, lookAhead));
                /* limit this to max buffer just to print out some of the buffer */
                pPacket->ActualLength =
                min(pPacket->ActualLength, pPacket->BufferLength);
                status = A_EPROTO;
                break;
            }

            if (pPacket->Endpoint
                    != HTC_GET_FIELD(pBuf, HTC_FRAME_HDR, ENDPOINTID)) {
                AR_DEBUG_PRINTF( ATH_DEBUG_ERR,
                        ("Refreshed HDR endpoint (%d) does not match expected endpoint (%d) \n", HTC_GET_FIELD(pBuf, HTC_FRAME_HDR, ENDPOINTID), pPacket->Endpoint));
                status = A_EPROTO;
                break;
            }
        }

        if (lookAhead != pPacket->PktInfo.AsRx.ExpectedHdr) {
            /* somehow the lookahead that gave us the full read length did not
             * reflect the actual header in the pending message */
            AR_DEBUG_PRINTF( ATH_DEBUG_ERR,
                    ("HTCProcessRecvHeader, lookahead mismatch! (pPkt:0x%lX flags:0x%X) \n", (unsigned long)pPacket, pPacket->PktInfo.AsRx.HTCRxFlags));
#ifdef ATH_DEBUG_MODULE
            DebugDumpBytes((A_UINT8 *)&pPacket->PktInfo.AsRx.ExpectedHdr,4,"Expected Message LookAhead");
            DebugDumpBytes(pBuf,sizeof(HTC_FRAME_HDR),"Current Frame Header");
#ifdef HTC_CAPTURE_LAST_FRAME
            DebugDumpBytes((A_UINT8 *)&target->LastFrameHdr,sizeof(HTC_FRAME_HDR),"Last Frame Header");
            if (target->LastTrailerLength != 0) {
                DebugDumpBytes(target->LastTrailer,
                        target->LastTrailerLength,
                        "Last trailer");
            }
#endif
#endif
            status = A_EPROTO;
            break;
        }

        /* get flags */
        temp = HTC_GET_FIELD(pBuf, HTC_FRAME_HDR, FLAGS);

        if (temp & HTC_FLAGS_RECV_TRAILER) {
            /* this packet has a trailer */

            /* extract the trailer length in control byte 0 */
            temp = HTC_GET_FIELD(pBuf, HTC_FRAME_HDR, CONTROLBYTES0);

            if ((temp < sizeof(HTC_RECORD_HDR)) || (temp > payloadLen)) {
                AR_DEBUG_PRINTF( ATH_DEBUG_ERR,
                        ("HTCProcessRecvHeader, invalid header (payloadlength should be :%d, CB[0] is:%d) \n", payloadLen, temp));
                status = A_EPROTO;
                break;
            }

            if (pPacket->PktInfo.AsRx.HTCRxFlags & HTC_RX_PKT_IGNORE_LOOKAHEAD) {
                /* this packet was fetched as part of an HTC bundle, the embedded lookahead is
                 * not valid since the next packet may have already been fetched as part of the
                 * bundle */
                pNextLookAheads = NULL;
                pNumLookAheads = NULL;
            }

            /* process trailer data that follows HDR + application payload */
            status = HIFDevProcessTrailer(pDev,
                    (pBuf + HTC_HDR_LENGTH + payloadLen - temp),
                    temp,
                    pNextLookAheads,
                    pNumLookAheads,
                    pPacket->Endpoint);

            if (A_FAILED(status)) {
                break;
            }

            /* trim length by trailer bytes */
            pPacket->ActualLength -= temp;
        }

        /* if we get to this point, the packet is good */
        /* remove header and adjust length */
        pPacket->pBuffer += HTC_HDR_LENGTH;
        pPacket->ActualLength -= HTC_HDR_LENGTH;

    }while (FALSE);

    if (A_FAILED(status)) {
        /* dump the whole packet */
        DebugDumpBytes(pBuf,
                pPacket->ActualLength < 256 ? pPacket->ActualLength : 256,
                "BAD HTC Recv PKT");
    } else {
        if (AR_DEBUG_LVL_CHECK(ATH_DEBUG_RECV)) {
            if (pPacket->ActualLength > 0) {
                AR_DEBUG_PRINTBUF(pPacket->pBuffer,pPacket->ActualLength,"HTC - Application Msg");
            }
        }
    }

    AR_DEBUG_PRINTF(ATH_DEBUG_RECV, ("-HTCProcessRecvHeader \n"));
    return status;
}
#endif

static A_STATUS HIFDevRecvMessagePendingHandler(HIF_SDIO_DEVICE *pDev,
        A_UINT8 MailBoxIndex, A_UINT32 MsgLookAheads[], int NumLookAheads,
        A_BOOL *pAsyncProc, int *pNumPktsFetched)
{
    A_STATUS status = A_OK;
    HTC_PACKET *pPacket;
    A_BOOL asyncProc = FALSE;
    A_UINT32 lookAheads[MAILBOX_LOOKAHEAD_SIZE_IN_WORD*HTC_HOST_MAX_MSG_PER_BUNDLE];
    int pktsFetched;
    HTC_PACKET_QUEUE recvPktQueue, syncCompletedPktsQueue;
    A_BOOL partialBundle;
    HTC_ENDPOINT_ID id;
    int totalFetched = 0;

    AR_DEBUG_PRINTF( ATH_DEBUG_RECV,
            ("+HTCRecvMessagePendingHandler NumLookAheads: %d \n", NumLookAheads));

    if (pNumPktsFetched != NULL) {
        *pNumPktsFetched = 0;
    }

    if (IS_DEV_IRQ_PROCESSING_ASYNC_ALLOWED(pDev)) {
        /* We use async mode to get the packets if the device layer supports it.
         * The device layer interfaces with HIF in which HIF may have restrictions on
         * how interrupts are processed */
        asyncProc = TRUE;
    }

    if (pAsyncProc != NULL) {
        /* indicate to caller how we decided to process this */
        *pAsyncProc = asyncProc;
    }

    if (NumLookAheads > HTC_HOST_MAX_MSG_PER_BUNDLE) {
        A_ASSERT(FALSE);
        return A_EPROTO;
    }

    /* on first entry copy the lookaheads into our temp array for processing */
    A_MEMCPY(lookAheads, MsgLookAheads, (sizeof(A_UINT32))*MAILBOX_LOOKAHEAD_SIZE_IN_WORD*NumLookAheads);

    while (TRUE) {

        /* reset packets queues */
        INIT_HTC_PACKET_QUEUE(&recvPktQueue);
        INIT_HTC_PACKET_QUEUE(&syncCompletedPktsQueue);

        if (NumLookAheads > HTC_HOST_MAX_MSG_PER_BUNDLE) {
            status = A_EPROTO;
            A_ASSERT(FALSE);
            break;
        }

        /* first lookahead sets the expected endpoint IDs for all packets in a bundle */
        id = ((HTC_FRAME_HDR *) &lookAheads[0])->EndpointID;

        if (id >= ENDPOINT_MAX) {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                    ("MsgPend, Invalid Endpoint in look-ahead: %d \n",id));
            status = A_EPROTO;
            break;
        }

        /* try to allocate as many HTC RX packets indicated by the lookaheads
         * these packets are stored in the recvPkt queue */
        status = HIFDevAllocAndPrepareRxPackets(pDev,
                lookAheads,
                NumLookAheads,
                &recvPktQueue);
        if (A_FAILED(status)) {
            break;
        }

        if (HTC_PACKET_QUEUE_DEPTH(&recvPktQueue) >= 2) {
            /* a recv bundle was detected, force IRQ status re-check again */
            //REF_IRQ_STATUS_RECHECK(&target->Device);
        }

        totalFetched += HTC_PACKET_QUEUE_DEPTH(&recvPktQueue);

        /* we've got packet buffers for all we can currently fetch,
         * this count is not valid anymore  */
        NumLookAheads = 0;
        partialBundle = FALSE;

        /* now go fetch the list of HTC packets */
        while (!HTC_QUEUE_EMPTY(&recvPktQueue)) {

            pktsFetched = 0;
#if 0
            if (target->RecvBundlingEnabled
                    && (HTC_PACKET_QUEUE_DEPTH(&recvPktQueue) > 1)) {
                /* there are enough packets to attempt a bundle transfer and recv bundling is allowed  */
                status = HIFDevIssueRecvPacketBundle(target, &recvPktQueue,
                        asyncProc ? NULL : &syncCompletedPktsQueue,
                        &pktsFetched, partialBundle);
                if (A_FAILED(status)) {
                    break;
                }

                if (HTC_PACKET_QUEUE_DEPTH(&recvPktQueue) != 0) {
                    /* we couldn't fetch all packets at one time, this creates a broken
                     * bundle  */
                    partialBundle = TRUE;
                }
            }
#endif
            /* see if the previous operation fetched any packets using bundling */
            if (0 == pktsFetched) {
                /* dequeue one packet */
                pPacket = HTC_PACKET_DEQUEUE(&recvPktQueue);
                A_ASSERT(pPacket != NULL);

                if (asyncProc) {
                    /* we use async mode to get the packet if the device layer supports it
                     * set our callback and context */
                    //pPacket->Completion = HTCRecvCompleteHandler;
                    //pPacket->pContext = target;
                } else {
                    /* fully synchronous */
                    pPacket->Completion = NULL;
                }

                if (HTC_PACKET_QUEUE_DEPTH(&recvPktQueue) > 0) {
                    /* lookaheads in all packets except the last one in the bundle must be ignored */
                    pPacket->PktInfo.AsRx.HTCRxFlags |=
                            HTC_RX_PKT_IGNORE_LOOKAHEAD;
                }

                /* Endpoint is used to save mailbox index */
                pPacket->Endpoint = (HTC_ENDPOINT_ID) MailBoxIndex;
                /* go fetch the packet */
                status = HIFDevRecvPacket(pDev, pPacket, pPacket->ActualLength);
                if (A_FAILED(status)) {
                    break;
                }
                if (!asyncProc) {
                    /* sent synchronously, queue this packet for synchronous completion */
                    HTC_PACKET_ENQUEUE(&syncCompletedPktsQueue, pPacket);
                }

            }

        }

        if (A_SUCCESS(status)) {
            //CheckRecvWaterMark(pEndpoint);
        }

        if (asyncProc) {
            /* we did this asynchronously so we can get out of the loop, the asynch processing
             * creates a chain of requests to continue processing pending messages in the
             * context of callbacks  */
            break;
        }

        /* synchronous handling */
        if (pDev->DSRCanYield) {
            /* for the SYNC case, increment count that tracks when the DSR should yield */
            pDev->CurrentDSRRecvCount++;
        }

        /* in the sync case, all packet buffers are now filled,
         * we can process each packet, check lookaheads and then repeat */

        /* unload sync completion queue */
        while (!HTC_QUEUE_EMPTY(&syncCompletedPktsQueue)) {
            A_UINT8 pipeid;
            adf_nbuf_t netbuf;
            //HTC_PACKET_QUEUE container;

            pPacket = HTC_PACKET_DEQUEUE(&syncCompletedPktsQueue);
            A_ASSERT(pPacket != NULL);

            //pEndpoint = &target->EndPoint[pPacket->Endpoint];
            /* reset count on each iteration, we are only interested in the last packet's lookahead
             * information when we break out of this loop */
            NumLookAheads = 0;
            /* process header for each of the recv packets
             * note: the lookahead of the last packet is useful for us to continue in this loop */
#if 0
            status = HIFDevProcessRecvHeader(pDev, pPacket, lookAheads,
                    &NumLookAheads);
            if (A_FAILED(status)) {
                break;
            }
#endif
            netbuf = (adf_nbuf_t) pPacket->pNetBufContext;
            /* set data length */
            adf_nbuf_put_tail(netbuf, pPacket->ActualLength);

            if (pDev->hif_callbacks.rxCompletionHandler) {
                pipeid = HIFDevMapMailBoxToPipe(pDev, MailBoxIndex, TRUE);
                pDev->hif_callbacks.rxCompletionHandler(pDev->hif_callbacks.Context,
                        netbuf,
                        pipeid);
            }
#if 0
            if (HTC_QUEUE_EMPTY(&syncCompletedPktsQueue)) {
                /* last packet's more packets flag is set based on the lookahead */
                SET_MORE_RX_PACKET_INDICATION_FLAG(lookAheads, NumLookAheads,
                        pEndpoint, pPacket);
            } else {
                /* packets in a bundle automatically have this flag set */
                FORCE_MORE_RX_PACKET_INDICATION_FLAG(pPacket);
            }
            /* good packet, indicate it */
            HTC_RX_STAT_PROFILE(target, pEndpoint, NumLookAheads);

            if (pPacket->PktInfo.AsRx.HTCRxFlags & HTC_RX_PKT_PART_OF_BUNDLE) {
                INC_HTC_EP_STAT(pEndpoint, RxPacketsBundled, 1);
            }

            INIT_HTC_PACKET_QUEUE_AND_ADD(&container, pPacket);
            DO_RCV_COMPLETION(pEndpoint, &container);
#endif
        }
        if (A_FAILED(status)) {
            break;
        }

        if (NumLookAheads == 0) {
            /* no more look aheads */
            break;
        }

        /* when we process recv synchronously we need to check if we should yield and stop
         * fetching more packets indicated by the embedded lookaheads */
#if 0
        if (pDev.DSRCanYield) {
            if (DEV_CHECK_RECV_YIELD(&target->Device)) {
                /* break out, don't fetch any more packets */
                break;
            }
        }
#endif
        /* check whether other OS contexts have queued any WMI command/data for WLAN.
         * This check is needed only if WLAN Tx and Rx happens in same thread context */
        A_CHECK_DRV_TX();

        /* for SYNCH processing, if we get here, we are running through the loop again due to a detected lookahead.
         * Set flag that we should re-check IRQ status registers again before leaving IRQ processing,
         * this can net better performance in high throughput situations */
        //REF_IRQ_STATUS_RECHECK(&target->Device);
    }
#if 0
    if (A_FAILED(status)) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                ("Failed to get pending recv messages (%d) \n",status));
        /* cleanup any packets we allocated but didn't use to actually fetch any packets */
        while (!HTC_QUEUE_EMPTY(&recvPktQueue)) {
            pPacket = HTC_PACKET_DEQUEUE(&recvPktQueue);
            /* clean up packets */
            HTC_RECYCLE_RX_PKT(target, pPacket,
                    &target->EndPoint[pPacket->Endpoint]);
        }
        /* cleanup any packets in sync completion queue */
        while (!HTC_QUEUE_EMPTY(&syncCompletedPktsQueue)) {
            pPacket = HTC_PACKET_DEQUEUE(&syncCompletedPktsQueue);
            /* clean up packets */
            HTC_RECYCLE_RX_PKT(target, pPacket,
                    &target->EndPoint[pPacket->Endpoint]);
        }
        if (HTC_STOPPING(target)) {
            AR_DEBUG_PRINTF(
                    ATH_DEBUG_WARN,
                    (" Host is going to stop. blocking receiver for HTCStop.. \n"));
            DevStopRecv(&target->Device,
                    asyncProc ? DEV_STOP_RECV_ASYNC : DEV_STOP_RECV_SYNC);
        }
    }

    /* before leaving, check to see if host ran out of buffers and needs to stop the
     * receiver */
    if (target->RecvStateFlags & HTC_RECV_WAIT_BUFFERS) {
        AR_DEBUG_PRINTF(
                ATH_DEBUG_WARN,
                (" Host has no RX buffers, blocking receiver to prevent overrun.. \n"));
        /* try to stop receive at the device layer */
        DevStopRecv(&target->Device,
                asyncProc ? DEV_STOP_RECV_ASYNC : DEV_STOP_RECV_SYNC);
    }
#endif
    if (pNumPktsFetched != NULL) {
        *pNumPktsFetched = totalFetched;
    }

    AR_DEBUG_PRINTF(ATH_DEBUG_RECV, ("-HTCRecvMessagePendingHandler \n"));
    return status;
}

static A_STATUS HIFDevServiceCPUInterrupt(HIF_SDIO_DEVICE *pDev)
{
    A_STATUS status;
    A_UINT8 cpu_int_status;
    A_UINT8 regBuffer[4];

    AR_DEBUG_PRINTF(ATH_DEBUG_IRQ, ("CPU Interrupt\n"));
    cpu_int_status = pDev->IrqProcRegisters.cpu_int_status
            & pDev->IrqEnableRegisters.cpu_int_status_enable;
    A_ASSERT(cpu_int_status);
    AR_DEBUG_PRINTF( ATH_DEBUG_IRQ,
            ("Valid interrupt source(s) in CPU_INT_STATUS: 0x%x\n", cpu_int_status));

    /* Clear the interrupt */
    pDev->IrqProcRegisters.cpu_int_status &= ~cpu_int_status; /* W1C */

    /* set up the register transfer buffer to hit the register 4 times , this is done
     * to make the access 4-byte aligned to mitigate issues with host bus interconnects that
     * restrict bus transfer lengths to be a multiple of 4-bytes */

    /* set W1C value to clear the interrupt, this hits the register first */
    regBuffer[0] = cpu_int_status;
    /* the remaining 4 values are set to zero which have no-effect  */
    regBuffer[1] = 0;
    regBuffer[2] = 0;
    regBuffer[3] = 0;

    status = HIFReadWrite(pDev->HIFDevice,
            CPU_INT_STATUS_ADDRESS,
            regBuffer,
            4,
            HIF_WR_SYNC_BYTE_FIX,
            NULL);

    A_ASSERT(status == A_OK);

    /* The Interrupt sent to the Host is generated via bit0 of CPU INT register*/
    if (cpu_int_status & 0x1){
        if(pDev && pDev->hif_callbacks.fwEventHandler)
        //this calls into HTC which propagates this to ol_target_failure()
        pDev->hif_callbacks.fwEventHandler(pDev->hif_callbacks.Context);
    }
    else
    AR_DEBUG_PRINTF( ATH_DEBUG_ERROR,
            ("%s: Unable to call fwEventHandler, invalid input arguments\n",__func__));

    return status;
}

static A_STATUS HIFDevServiceErrorInterrupt(HIF_SDIO_DEVICE *pDev)
{
    A_STATUS status;
    A_UINT8 error_int_status;
    A_UINT8 regBuffer[4];

    AR_DEBUG_PRINTF(ATH_DEBUG_IRQ, ("Error Interrupt\n"));
    error_int_status = pDev->IrqProcRegisters.error_int_status & 0x0F;
    A_ASSERT(error_int_status);
    AR_DEBUG_PRINTF( ATH_DEBUG_IRQ,
            ("Valid interrupt source(s) in ERROR_INT_STATUS: 0x%x\n", error_int_status));

    if (ERROR_INT_STATUS_WAKEUP_GET(error_int_status)) {
        /* Wakeup */
        AR_DEBUG_PRINTF(ATH_DEBUG_IRQ, ("Error : Wakeup\n"));
    }

    if (ERROR_INT_STATUS_RX_UNDERFLOW_GET(error_int_status)) {
        /* Rx Underflow */
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("Error : Rx Underflow\n"));
    }

    if (ERROR_INT_STATUS_TX_OVERFLOW_GET(error_int_status)) {
        /* Tx Overflow */
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("Error : Tx Overflow\n"));
    }

    /* Clear the interrupt */
    pDev->IrqProcRegisters.error_int_status &= ~error_int_status; /* W1C */

    /* set up the register transfer buffer to hit the register 4 times , this is done
     * to make the access 4-byte aligned to mitigate issues with host bus interconnects that
     * restrict bus transfer lengths to be a multiple of 4-bytes */

    /* set W1C value to clear the interrupt, this hits the register first */
    regBuffer[0] = error_int_status;
    /* the remaining 4 values are set to zero which have no-effect  */
    regBuffer[1] = 0;
    regBuffer[2] = 0;
    regBuffer[3] = 0;

    status = HIFReadWrite(pDev->HIFDevice,
            ERROR_INT_STATUS_ADDRESS,
            regBuffer,
            4,
            HIF_WR_SYNC_BYTE_FIX,
            NULL);

    A_ASSERT(status == A_OK);
    return status;
}

static A_STATUS HIFDevServiceDebugInterrupt(HIF_SDIO_DEVICE *pDev)
{
    A_UINT32 dummy;
    A_STATUS status;

    /* Send a target failure event to the application */
    AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("Target debug interrupt\n"));

    /* clear the interrupt , the debug error interrupt is
     * counter 0 */
    /* read counter to clear interrupt */
    status = HIFReadWrite(pDev->HIFDevice,
            COUNT_DEC_ADDRESS,
            (A_UINT8 *) &dummy,
            4,
            HIF_RD_SYNC_BYTE_INC,
            NULL);

    A_ASSERT(status == A_OK);
    return status;
}

static A_STATUS HIFDevServiceCounterInterrupt(HIF_SDIO_DEVICE *pDev)
{
    A_UINT8 counter_int_status;

    AR_DEBUG_PRINTF(ATH_DEBUG_IRQ, ("Counter Interrupt\n"));

    counter_int_status = pDev->IrqProcRegisters.counter_int_status
            & pDev->IrqEnableRegisters.counter_int_status_enable;

    AR_DEBUG_PRINTF( ATH_DEBUG_IRQ,
            ("Valid interrupt source(s) in COUNTER_INT_STATUS: 0x%x\n", counter_int_status));

    /* Check if the debug interrupt is pending
     * NOTE: other modules like GMBOX may use the counter interrupt for
     * credit flow control on other counters, we only need to check for the debug assertion
     * counter interrupt */
    if (counter_int_status & AR6K_TARGET_DEBUG_INTR_MASK) {
        return HIFDevServiceDebugInterrupt(pDev);
    }

    return A_OK;
}

/* process pending interrupts synchronously */
static A_STATUS HIFDevProcessPendingIRQs(HIF_SDIO_DEVICE *pDev, A_BOOL *pDone,
        A_BOOL *pASyncProcessing)
{
    A_STATUS status = A_OK;
    A_UINT8 host_int_status = 0;
    A_UINT32 lookAhead[MAILBOX_LOOKAHEAD_SIZE_IN_WORD*MAILBOX_USED_COUNT];
    int i;

    A_MEMZERO(&lookAhead, sizeof(lookAhead));
    AR_DEBUG_PRINTF(ATH_DEBUG_IRQ,
            ("+ProcessPendingIRQs: (dev: 0x%lX)\n", (unsigned long)pDev));

    /*** NOTE: the HIF implementation guarantees that the context of this call allows
     *         us to perform SYNCHRONOUS I/O, that is we can block, sleep or call any API that
     *         can block or switch thread/task ontexts.
     *         This is a fully schedulable context.
     * */
    do {

        if (pDev->IrqEnableRegisters.int_status_enable == 0) {
            /* interrupt enables have been cleared, do not try to process any pending interrupts that
             * may result in more bus transactions.  The target may be unresponsive at this
             * point. */
            break;
        }
        /*
         * Read the first 28 bytes of the HTC register table. This will yield us
         * the value of different int status registers and the lookahead
         * registers.
         *    length = sizeof(int_status) + sizeof(cpu_int_status) +
         *             sizeof(error_int_status) + sizeof(counter_int_status) +
         *             sizeof(mbox_frame) + sizeof(rx_lookahead_valid) +
         *             sizeof(hole) +  sizeof(rx_lookahead) +
         *             sizeof(int_status_enable) + sizeof(cpu_int_status_enable) +
         *             sizeof(error_status_enable) +
         *             sizeof(counter_int_status_enable);
         *
         */
        status = HIFReadWrite(pDev->HIFDevice,
                HOST_INT_STATUS_ADDRESS,
                (A_UINT8 *) &pDev->IrqProcRegisters,
                sizeof(pDev->IrqProcRegisters),
                HIF_RD_SYNC_BYTE_INC,
                NULL);

        if (A_FAILED(status)) {
            break;
        }

        if (AR_DEBUG_LVL_CHECK(ATH_DEBUG_IRQ)) {
            HIFDevDumpRegisters(pDev,
                    &pDev->IrqProcRegisters,
                    &pDev->IrqEnableRegisters);
        }

        /* Update only those registers that are enabled */
        host_int_status = pDev->IrqProcRegisters.host_int_status
                & pDev->IrqEnableRegisters.int_status_enable;

        /* only look at mailbox status if the HIF layer did not provide this function,
         * on some HIF interfaces reading the RX lookahead is not valid to do */
        for (i = 0; i < MAILBOX_USED_COUNT; i++) {
            lookAhead[MAILBOX_LOOKAHEAD_SIZE_IN_WORD*i] = lookAhead[MAILBOX_LOOKAHEAD_SIZE_IN_WORD*i+1] = 0;
            if (host_int_status & (1 << i)) {
                /* mask out pending mailbox value, we use "lookAhead" as the real flag for
                 * mailbox processing below */
                host_int_status &= ~(1 << i);
                if (pDev->IrqProcRegisters.rx_lookahead_valid & (1 << i)) {
                    /* mailbox has a message and the look ahead is valid */
                    lookAhead[MAILBOX_LOOKAHEAD_SIZE_IN_WORD*i] = pDev->IrqProcRegisters.rx_lookahead[MAILBOX_LOOKAHEAD_SIZE_IN_WORD*i];
                    lookAhead[MAILBOX_LOOKAHEAD_SIZE_IN_WORD*i+1] = pDev->IrqProcRegisters.rx_lookahead[MAILBOX_LOOKAHEAD_SIZE_IN_WORD*i+1];
                }
            }
        } /*end of for loop*/


    } while (FALSE);

    do {

        A_BOOL bLookAheadValid = FALSE;
        /* did the interrupt status fetches succeed? */
        if (A_FAILED(status)) {
            break;
        }

        for (i = 0; i < MAILBOX_USED_COUNT; i++) {
            if (lookAhead[MAILBOX_LOOKAHEAD_SIZE_IN_WORD*i] != 0) {
                bLookAheadValid = TRUE;
                break;
            }
        }

        if ((0 == host_int_status) && !bLookAheadValid) {
            /* nothing to process, the caller can use this to break out of a loop */
            *pDone = TRUE;
            break;
        }

        if (bLookAheadValid) {
            for (i = 0; i < MAILBOX_USED_COUNT; i++) {
                int fetched = 0;

                if (lookAhead[MAILBOX_LOOKAHEAD_SIZE_IN_WORD*i] == 0) {
                    continue;
                }AR_DEBUG_PRINTF(ATH_DEBUG_IRQ,
                        ("Pending mailbox[%d] message, LookAhead: 0x%X\n", i, lookAhead[i]));
                /* Mailbox Interrupt, the HTC layer may issue async requests to empty the
                 * mailbox...
                 * When emptying the recv mailbox we use the async handler above called from the
                 * completion routine of the callers read request. This can improve performance
                 * by reducing context switching when we rapidly pull packets */
                status = HIFDevRecvMessagePendingHandler(pDev,
                        i,
                        &lookAhead[MAILBOX_LOOKAHEAD_SIZE_IN_WORD*i],
                        1,
                        pASyncProcessing,
                        &fetched);
                if (A_FAILED(status)) {
                    break;
                }

                if (!fetched) {
                    /* HTC could not pull any messages out due to lack of resources */
                    /* force DSR handler to ack the interrupt */
                    *pASyncProcessing = FALSE;
                    pDev->RecheckIRQStatusCnt = 0;
                }
            }
        }

        /* now handle the rest of them */
        AR_DEBUG_PRINTF( ATH_DEBUG_IRQ,
                (" Valid interrupt source(s) for OTHER interrupts: 0x%x\n", host_int_status));

        if (HOST_INT_STATUS_CPU_GET(host_int_status)) {
            /* CPU Interrupt */
            status = HIFDevServiceCPUInterrupt(pDev);
            if (A_FAILED(status)) {
                break;
            }
        }

        if (HOST_INT_STATUS_ERROR_GET(host_int_status)) {
            /* Error Interrupt */
            status = HIFDevServiceErrorInterrupt(pDev);
            if (A_FAILED(status)) {
                break;
            }
        }

        if (HOST_INT_STATUS_COUNTER_GET(host_int_status)) {
            /* Counter Interrupt */
            status = HIFDevServiceCounterInterrupt(pDev);
            if (A_FAILED(status)) {
                break;
            }
        }

    } while (FALSE);

    /* an optimization to bypass reading the IRQ status registers unecessarily which can re-wake
     * the target, if upper layers determine that we are in a low-throughput mode, we can
     * rely on taking another interrupt rather than re-checking the status registers which can
     * re-wake the target.
     *
     * NOTE : for host interfaces that use the special GetPendingEventsFunc, this optimization cannot
     * be used due to possible side-effects.  For example, SPI requires the host to drain all
     * messages from the mailbox before exiting the ISR routine. */
    if (!(*pASyncProcessing) && (pDev->RecheckIRQStatusCnt == 0)) {
        AR_DEBUG_PRINTF(ATH_DEBUG_IRQ,
                ("Bypassing IRQ Status re-check, forcing done \n"));
        *pDone = TRUE;
    }

    AR_DEBUG_PRINTF( ATH_DEBUG_IRQ,
            ("-ProcessPendingIRQs: (done:%d, async:%d) status=%d \n", *pDone, *pASyncProcessing, status));

    return status;
}

#define DEV_CHECK_RECV_YIELD(pDev) \
            ((pDev)->CurrentDSRRecvCount >= (pDev)->HifIRQYieldParams.RecvPacketYieldCount)

/* Synchronousinterrupt handler, this handler kicks off all interrupt processing.*/
A_STATUS HIFDevDsrHandler(void *context)
{
    HIF_SDIO_DEVICE *pDev = (HIF_SDIO_DEVICE *) context;
    A_STATUS status = A_OK;
    A_BOOL done = FALSE;
    A_BOOL asyncProc = FALSE;

    AR_DEBUG_PRINTF(ATH_DEBUG_IRQ,
            ("+DevDsrHandler: (dev: 0x%lX)\n", (unsigned long)pDev));

    /* reset the recv counter that tracks when we need to yield from the DSR */
    pDev->CurrentDSRRecvCount = 0;
    /* reset counter used to flag a re-scan of IRQ status registers on the target */
    pDev->RecheckIRQStatusCnt = 0;

    while (!done) {
        status = HIFDevProcessPendingIRQs(pDev, &done, &asyncProc);
        if (A_FAILED(status)) {
            break;
        }

        if (HIF_DEVICE_IRQ_SYNC_ONLY == pDev->HifIRQProcessingMode) {
            /* the HIF layer does not allow async IRQ processing, override the asyncProc flag */
            asyncProc = FALSE;
            /* this will cause us to re-enter ProcessPendingIRQ() and re-read interrupt status registers.
             * this has a nice side effect of blocking us until all async read requests are completed.
             * This behavior is required on some HIF implementations that do not allow ASYNC
             * processing in interrupt handlers (like Windows CE) */

            if (pDev->DSRCanYield && DEV_CHECK_RECV_YIELD(pDev)) {
                /* ProcessPendingIRQs() pulled enough recv messages to satisfy the yield count, stop
                 * checking for more messages and return */
                break;
            }
        }

        if (asyncProc) {
            /* the function performed some async I/O for performance, we
             need to exit the ISR immediately, the check below will prevent the interrupt from being
             Ack'd while we handle it asynchronously */
            break;
        }

    }

    if (A_SUCCESS(status) && !asyncProc) {
        /* Ack the interrupt only if :
         *  1. we did not get any errors in processing interrupts
         *  2. there are no outstanding async processing requests */
        if (pDev->DSRCanYield) {
            /* if the DSR can yield do not ACK the interrupt, there could be more pending messages.
             * The HIF layer must ACK the interrupt on behalf of HTC */
            AR_DEBUG_PRINTF( ATH_DEBUG_IRQ,
                    (" Yield in effect (cur RX count: %d) \n", pDev->CurrentDSRRecvCount));
        } else {
            AR_DEBUG_PRINTF(ATH_DEBUG_IRQ,
                    (" Acking interrupt from DevDsrHandler \n"));
            HIFAckInterrupt(pDev->HIFDevice);
        }
    }

    AR_DEBUG_PRINTF(ATH_DEBUG_IRQ, ("-DevDsrHandler \n"));
    return status;
}
