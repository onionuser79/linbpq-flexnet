/*
Copyright 2001-2022 John Wiseman G8BPQ

This file is part of LinBPQ/BPQ32.

LinBPQ/BPQ32 is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

LinBPQ/BPQ32 is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with LinBPQ/BPQ32.  If not, see http://www.gnu.org/licenses
*/	



// Monitor Code - from moncode.asm

#pragma data_seg("_BPQDATA")

#define _CRT_SECURE_NO_DEPRECATE 

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma data_seg("_BPQDATA")
				
#include "cheaders.h"
#include "tncinfo.h"

extern char NCMPTypes[6][28];

//	MSGFLAG contains CMD/RESPONSE BITS

#define	CMDBIT	4		// CURRENT MESSAGE IS A COMMAND
#define	RESP 2		// CURRENT MSG IS RESPONSE
#define	VER1 1 		// CURRENT MSG IS VERSION 1


#define	UI	3
#define	SABM 0x2F
#define	DISC 0x43
#define	DM	0x0F
#define	UA	0x63
#define	FRMR 0x87
#define	RR	1
#define	RNR	5
#define	REJ	9

#define	PFBIT 0x10		// POLL/FINAL BIT IN CONTROL BYTE

#define	NETROM_PID 0xCF
#define	IP_PID 0xCC
#define	ARP_PID 0xCD

#define	NODES_SIG	0xFF

UCHAR * DisplayINP3RIF(UCHAR * ptr1, UCHAR * ptr2, int msglen);

char * DISPLAY_NETROM(MESSAGE * ADJBUFFER, UCHAR * Output, int MsgLen);
UCHAR * DISPLAYIPDATAGRAM(IPMSG * IP, UCHAR * Output, int Avail);
char * DISPLAYARPDATAGRAM(UCHAR * Datagram, UCHAR * Output);


DllExport int APIENTRY SetTraceOptions(int mask, int mtxparam, int mcomparam)
{

//	Sets the tracing options for DecodeFrame. Mask is a bit
//	mask of ports to monitor (ie 101 binary will monitor ports
//	1 and 3). MTX enables monitoring on transmitted frames. MCOM
//	enables monitoring of protocol control frames (eg SABM, UA, RR),
//	as well as info frames.

//  *** For external use only, supports portnum up to 31 ***

	MMASK = mask;
	MTX = mtxparam;
	MCOM = mcomparam;

	return (0);
}

DllExport int APIENTRY SetTraceOptions64(uint64_t mask, int mtxparam, int mcomparam, int monUIOnly)
{

//	Sets the tracing options for DecodeFrame. Mask is a bit
//	mask of ports to monitor (ie 101 binary will monitor ports
//	1 and 3). MTX enables monitoring on transmitted frames. MCOM
//	enables monitoring of protocol control frames (eg SABM, UA, RR),
//	as well as info frames.

//  *** For external use only, supports portnum up to 63 ***

	MMASK = mask;
	MTX = mtxparam;
	MCOM = mcomparam;
	MUIONLY = monUIOnly;

	return (0);
}
DllExport int APIENTRY SetTraceOptionsEx(int mask, int mtxparam, int mcomparam, int monUIOnly)
{

//  *** For external use only, supports portnum up to 31 ***

//	Sets the tracing options for DecodeFrame. Mask is a bit
//	mask of ports to monitor (ie 101 binary will monitor ports
//	1 and 3). MTX enables monitoring on transmitted frames. MCOM
//	enables monitoring of protocol control frames (eg SABM, UA, RR),
//	as well as info frames.


	MMASK = mask;
	MTX = mtxparam;
	MCOM = mcomparam;
	MUIONLY = monUIOnly;

	return 0;
}

int IntSetTraceOptionsEx(uint64_t mask, int mtxparam, int mcomparam, int monUIOnly)
{

//	Sets the tracing options for DecodeFrame. Mask is a bit
//	mask of ports to monitor (ie 101 binary will monitor ports
//	1 and 3). MTX enables monitoring on transmitted frames. MCOM
//	enables monitoring of protocol control frames (eg SABM, UA, RR),
//	as well as info frames.


	MMASK = mask;
	MTX = mtxparam;
	MCOM = mcomparam;
	MUIONLY = monUIOnly;

	return 0;
}

int APRSDecodeFrame(MESSAGE * msg, char * buffer, time_t Stamp, uint64_t Mask)
{
	return IntDecodeFrame(msg, buffer, Stamp, Mask, TRUE, FALSE);
}
DllExport int APIENTRY DecodeFrame(MESSAGE * msg, char * buffer, time_t Stamp)
{
	return IntDecodeFrame(msg, buffer, Stamp, MMASK, FALSE, FALSE);
}

int IntDecodeFrame(MESSAGE * msg, char * buffer, time_t Stamp, uint64_t Mask, BOOL APRS, BOOL MINI)
{
	UCHAR * ptr;
	int n;
	MESSAGE * ADJBUFFER;
	ptrdiff_t Work;
	UCHAR CTL;
	BOOL PF = 0;
	char CRCHAR[3] = "  ";
	char PFCHAR[3] = "  ";
	int Port;
	int MSGFLAG = 0;		//CR and V1 flags
	char * Output = buffer;
	char TR = 'R';
	char From[10], To[10];
	BOOL Info = 0;
	BOOL FRMRFLAG = 0;
	BOOL XIDFLAG = 0;
	BOOL TESTFLAG = 0;

	size_t MsgLen = msg->LENGTH;

	// Use gmtime so we can also display in local time

	struct tm * TM;

	if (MTX & 0x80)
		TM = localtime(&Stamp);
	else
		TM = gmtime(&Stamp);

	// MINI mode is for Node Listen (remote monitor) Mode. Keep info to minimum
/*
KO6IZ*>K7TMG-1:
/ex
KO6IZ*>K7TMG-1:
b
KO6IZ*>K7TMG-1 (UA)
W0TX*>KC6OAR>KB9KC>ID:
W0TX/R DRC/D W0TX-2/G W0TX-1/B W0TX-7/N
KC6OAR*>ID:
*/
	// Check Port

	Port = msg->PORT;

	if (Port == 40)
		Port = Port;
	
	if (Port & 0x80)
	{
		if ((MTX & 1) == 0)
			return 0;							//	TRANSMITTED FRAME - SEE IF MTX ON
		
		TR = 'T';
	}

	Port &= 0x7F;

	if ((((uint64_t)1 << (Port - 1)) & Mask) == 0)		// Check MMASK
	{
		if (msg->Padding[0] == '[')
			msg->Padding[0] = 0;

		return 0;
	}
	

	// We now pass Text format monitoring from non-ax25 drivers through this code
	// As a valid ax.25 address must have bottom bit set flag plain text messages
	// with hex 01.

	//	GET THE CONTROL BYTE, TO SEE IF THIS FRAME IS TO BE DISPLAYED

	if (msg->DEST[0] == 1)
	{
		// Just copy text (Null Terminated) to output

		// Need Timestamp and T/R

		// Add Port: unless Mail Mon (port 64)

		Output += sprintf((char *)Output, "%02d:%02d:%02d%c ", TM->tm_hour, TM->tm_min, TM->tm_sec, TR);

		strcpy(Output, &msg->DEST[1]);
		Output += strlen(Output);

		if (buffer[strlen(buffer) -1] == '\r')
			Output--;

		if (Port == 64)
			Output += sprintf((char *)Output, " \r");
		else
			Output += sprintf((char *)Output, " Port=%d\r", Port);
		
		return (int)strlen(buffer);
	}

	n = 8;						// MAX DIGIS
	ptr = &msg->ORIGIN[6];	// End of Address bit

	while ((*ptr & 1) == 0)
	{
		//	MORE TO COME
	
		ptr += 7;
		n--;

		if (n < 0)
			return 0;						// Corrupt - no end of address bit
	}

	// Reached End of digis

	Work = ptr - &msg->ORIGIN[6];			// Work is length of digis

	MsgLen -= Work;

	ADJBUFFER = (MESSAGE *)((UCHAR *)msg + Work);			// ADJBUFFER points to CTL, etc. allowing for digis

	CTL = ADJBUFFER->CTL;

	if (CTL & PFBIT)
		PF = TRUE;

	CTL &= ~PFBIT;

	if (MUIONLY)
		if (CTL != 3)
			return 0;

	if ((CTL & 1) == 0 || CTL == FRMR || CTL == 3)
	{
	}
	else
	{
		if (((CTL & 2) && MINI) == 0)		// Want Control (but not super unless MCOM
			if (MCOM == 0)
				return 0;						// Dont do control
	}


	// Add Port: if MINI mode and monitoring more than one port

	if (MINI == 0)
		Output += sprintf((char *)Output, "%02d:%02d:%02d%c ", TM->tm_hour, TM->tm_min, TM->tm_sec, TR);
	else
		if (CountBits64(Mask) > 1)
			Output += sprintf((char *)Output, "%d:", Port);

	From[ConvFromAX25(msg->ORIGIN, From)] = 0;
	To[ConvFromAX25(msg->DEST, To)] = 0;

	Output += sprintf((char *)Output, "%s>%s", From, To);

	//	Display any Digi-Peaters   

	n = 8;					// Max number of digi-peaters
	ptr = &msg->ORIGIN[6];	// End of Address bit

	while ((*ptr & 1) == 0)
	{
		//	MORE TO COME

		From[ConvFromAX25(ptr + 1, From)] = 0;
		Output += sprintf((char *)Output, ",%s", From);
	
		ptr += 7;
		n--;

		if (n == 0)
			break;

		// See if digi actioned - put a * on last actioned

		if (*ptr & 0x80)
		{
			if (*ptr & 1)						// if last address, must need *
				*(Output++) = '*';
			else
				if ((ptr[7] & 0x80) == 0)		// Repeased by next?
					*(Output++) = '*';			// No, so need *
		}
	}		
	
	if (MINI == 0)
		Output += sprintf((char *)Output, " Port=%d ", Port);

	// Set up CR and PF

	CRCHAR[0] = 0;
	PFCHAR[0] = 0;

	if (msg->DEST[6] & 0x80)
	{
		if (msg->ORIGIN[6] & 0x80)			//	Both set, assume V1
			MSGFLAG |= VER1;
		else
		{
			MSGFLAG |= CMDBIT;
			CRCHAR[0] = ' ';
			CRCHAR[1] = 'C';
			if (PF)							// If FP set
			{
				PFCHAR[0] = ' ';
				PFCHAR[1] = 'P';
			}
		}
	}
	else
	{
		if (msg->ORIGIN[6] & 0x80)			//	Only Origin Set
		{
			MSGFLAG |= RESP;
			CRCHAR[0] = ' ';
			CRCHAR[1] = 'R';
			if (PF)							// If FP set
			{
				PFCHAR[0] = ' ';
				PFCHAR[1] = 'F';
			}
		}
		else
			MSGFLAG |= VER1;				// Neither, assume V1
	}

	if ((CTL & 1) == 0)						// I frame
	{
		int NS = (CTL >> 1) & 7;			// ISOLATE RECEIVED N(S)
		int NR = (CTL >> 5) & 7;

		Info = 1;

		if (MINI == 0)
			Output += sprintf((char *)Output, "<I%s%s S%d R%d>", CRCHAR, PFCHAR, NS, NR);
	}
	else if (CTL == 3)
	{
		//	Un-numbered Information Frame 

		Output += sprintf((char *)Output, "<UI%s>", CRCHAR);
		Info = 1;
	}
	else if (CTL & 2)
	{
		// UN Numbered
				
		char SUP[6] = "??";

		switch (CTL)
		{
		case SABM:

			strcpy(SUP, "C");
			break;

		case SABME:

			strcpy(SUP, "SABME");
			break;

		case XID:

			strcpy(SUP, "XID");
			XIDFLAG = 1;
			break;

		case TEST:

			strcpy(SUP, "TEST");
			TESTFLAG = 1;
			break;

		case DISC:

			strcpy(SUP, "D");
			break;

		case DM:

			strcpy(SUP, "DM");
			break;
		
		case UA:

			strcpy(SUP, "UA");
			break;
		

		case FRMR:

			strcpy(SUP, "FRMR");
			FRMRFLAG = 1;
			break;
		}

		if (MINI)
			Output += sprintf((char *)Output, "<%s>", SUP);
		else
			Output += sprintf((char *)Output, "<%s%s%s>", SUP, CRCHAR, PFCHAR);
	}
	else
	{
		// Super

		int NR = (CTL >> 5) & 7;
		char SUP[5] = "??";

		switch (CTL & 0x0F)
		{
		case RR:

			strcpy(SUP, "RR");
			break;

		case RNR:

			strcpy(SUP, "RNR");
			break;

		case REJ:

			strcpy(SUP, "REJ");
			break;
		case SREJ:

			strcpy(SUP, "SREJ");
			break;
		}

		Output += sprintf((char *)Output, "<%s%s%s R%d>", SUP, CRCHAR, PFCHAR, NR);

	}

	if (FRMRFLAG)
		Output += sprintf((char *)Output, " %02X %02X %02X", ADJBUFFER->PID, ADJBUFFER->L2DATA[0], ADJBUFFER->L2DATA[1]); 

	if (XIDFLAG)
	{
		// Decode and display XID

		UCHAR * ptr = &ADJBUFFER->PID;

		if (*ptr++ == 0x82 && *ptr++ == 0x80)
		{
			int Type;
			int Len;
			unsigned int value;
			int xidlen = *(ptr++) << 8;
			xidlen += *ptr++;
		
			// XID is set of Type, Len, Value n-tuples

// G8BPQ-2>G8BPQ:(XID cmd, p=1) Half-Duplex SREJ modulo-128 I-Field-Length-Rx=256 Window-Size-Rx=32 Ack-Timer=3000 Retries=10


			while (xidlen > 0)
			{
				Type = *ptr++;
				Len = *ptr++;

				value = 0;
				xidlen -= (Len + 2);

				while (Len--)
				{
					value <<=8;
					value += *ptr++;
				}
				switch(Type)
				{
				case 2:				//Bin fields
				case 3:

					Output += sprintf((char *)Output, " %d=%x", Type, value);
					break;

				case 6:				//RX Size

					Output += sprintf((char *)Output, " RX Paclen=%d", value / 8);
					break;

				case 8:				//RX Window

					Output += sprintf((char *)Output, " RX Window=%d", value);
					break;

				case 16:

					Output += sprintf((char *)Output, " Can Compress");
					break;

				case 17:

					Output += sprintf((char *)Output, " Compress ok");
					break;
				}
			}	
		}
	}

	if (msg->Padding[0] == '[')
		Output += sprintf((char *)Output, " %s", msg->Padding);

	msg->Padding[0] = 0;

	if (Info)
	{
		// We have an info frame

		switch (ADJBUFFER->PID)
		{
		case 0xF1:
		case 0xF2:

			//	Compressed L2 Data

		Output += sprintf((char *)Output, " <%d Bytes of Compressed L2 Data>", MsgLen - (19 + sizeof(void *)));

		break;

		case 0xF0:		// Normal Data
		{
			char Infofield[257];
			char * ptr1 = Infofield;
			char * ptr2 = ADJBUFFER->L2DATA;
			UCHAR C;
			size_t len;

			MsgLen = MsgLen - (19 + sizeof(void *));

			if (MsgLen < 0 || MsgLen > 257)
				return 0;				// Duff

			while (MsgLen--)
			{
				C = *(ptr2++);

				if (APRS)
					*(ptr1++) = C;		// MIC-E needs all chars
				else
				{
					// Convert to printable

					C &= 0x7F;

					if (C == 13 || C == 10 || C > 31)
						*(ptr1++) = C;	
				}
			}

			len = ptr1 - Infofield;
	
			Output[0] = ':';
			Output[1] = 13;
			memcpy(&Output[2], Infofield, len);
			Output += (len + 2);

			break;
		}
		case NETROM_PID:
			
			Output = DISPLAY_NETROM(ADJBUFFER, Output, (int)MsgLen);
			break;

		case IP_PID:

			Output += sprintf((char *)Output, " <IP>\r");
			Output = DISPLAYIPDATAGRAM((IPMSG *)&ADJBUFFER->L2DATA[0], Output,
				(int)MsgLen - (int)(19 + sizeof(void *)));
			break;

		case ARP_PID:
			
			Output = DISPLAYARPDATAGRAM(&ADJBUFFER->L2DATA[0], Output);
			break;

		case 8:					// Fragmented IP

			n = ADJBUFFER->L2DATA[0];	// Frag Count

			Output += sprintf((char *)Output, "<Fragmented IP %02x>\r", n);

			if (ADJBUFFER->L2DATA[0] & 0x80)	// First Frag - Display Header
			{
				Output = DISPLAYIPDATAGRAM((IPMSG *)&ADJBUFFER->L2DATA[2], Output,
					(int)MsgLen - (int)(19 + sizeof(void *)) - 2);
			}

			break;	
		}
	}

	if (Output[-1] != 13)
		Output += sprintf((char *)Output, "\r");

	return (int)(Output - buffer);

}
//      Display NET/ROM data                                                 

char * DISPLAY_NETROM(MESSAGE * ADJBUFFER, UCHAR * Output, int MsgLen)
{
	char Alias[7]= "";
	char Dest[10];
	char Node[10];
	UCHAR TTL, Index, ID, TXNO, RXNO, OpCode, Flags, Window;
	UCHAR * ptr = &ADJBUFFER->L2DATA[0];
	int service = 0;
	int netromx = 0;		// Set if Paula's connect to service

 	if (ADJBUFFER->L2DATA[0] == NODES_SIG)
	{
		// Display NODES

		// If an INP3 RIF (type <> UI) decode as such
	
		if (ADJBUFFER->CTL != 3)		// UI
			return DisplayINP3RIF(&ADJBUFFER->L2DATA[1], Output, MsgLen - (MSGHDDRLEN + 14 + 3));

		memcpy(Alias, ++ptr, 6);

		ptr += 6;
	
		Output += sprintf((char *)Output, " NODES broadcast from %s\r", Alias);

		MsgLen -= 30;					//Header, mnemonic and signature length

		while(MsgLen > 20)				// Entries are 21 bytes
		{
			Dest[ConvFromAX25(ptr, Dest)] = 0;
			ptr +=7;
			memcpy(Alias, ptr, 6);
			ptr +=6;
			strlop(Alias, ' ');
			Node[ConvFromAX25(ptr, Node)] = 0;
			ptr +=7;

			Output += sprintf((char *)Output, "  %s:%s via %s qlty=%d\r", Alias, Dest, Node, ptr[0]);
			ptr++;
			MsgLen -= 21;
		}
		return Output;
	}

	if (ADJBUFFER->L2DATA[0] == 0xfe)			// Paula's Nodes Poll
	{
		memcpy(Alias, ++ptr, 6);
		Output += sprintf((char *)Output, " NODES POLL from %s\r", Alias);
		return Output;
	}


	//	Display normal NET/ROM transmissions 

	Output += sprintf((char *)Output, " NET/ROM\r  ");

	Dest[ConvFromAX25(ptr, Dest)] = 0;
	ptr +=7;
	Node[ConvFromAX25(ptr, Node)] = 0;
	ptr +=7;

	TTL = *(ptr++);
	Index = *(ptr++);
	ID = *(ptr++);
	TXNO = *(ptr++);
	RXNO = *(ptr++);
	OpCode = Flags = *(ptr++);

	OpCode &= 15;				// Remove Flags

	Output += sprintf((char *)Output, "%s to %s ttl %d cct=%02X%02X ", Dest, Node, TTL, Index, ID );
	MsgLen -= 20;

	switch (OpCode)
	{

	case L4CREQX:			// Paula's connect to service

		netromx = 1;
		service = (RXNO << 8) | TXNO;

	case L4CREQ:

		Window = *(ptr++);
		Dest[ConvFromAX25(ptr, Dest)] = 0;
		ptr +=7;
		Node[ConvFromAX25(ptr, Node)] = 0;
		ptr +=7;

		if (netromx)
			Output += sprintf((char *)Output, "<CON REQX> w=%d %d@%s at %s", Window, service, Dest, Node);
		else
			Output += sprintf((char *)Output, "<CON REQ> w=%d %s at %s", Window, Dest, Node);

		if (MsgLen > 38)				// BPQ Extended Params
		{
			short Timeout = (SHORT)*ptr;
			Output += sprintf((char *)Output, " t/o %d", Timeout);
		}

		return Output;

	case L4CACK:

		if (Flags & L4BUSY)				// BUSY RETURNED
			return Output + sprintf((char *)Output, " <CON NAK> - BUSY");

		return Output + sprintf((char *)Output, " <CON ACK> w=%d my cct=%02X%02X", ptr[0], TXNO, RXNO);

	case L4DREQ:

		return Output + sprintf((char *)Output, " <DISC REQ>");

	case L4DACK:

		return Output + sprintf((char *)Output, " <DISC ACK>");

	case L4RESET:

		return Output + sprintf((char *)Output, " <RESET>");

	case L4INFO:
		{
			char Infofield[257];
			char * ptr1 = Infofield;
			UCHAR C;
			size_t len;
			
			Output += sprintf((char *)Output, " <INFO S%d R%d>", TXNO, RXNO);
			
			if (Flags & L4BUSY)
				*(Output++) = 'B';

			if (Flags & L4NAK)
				*(Output++) = 'N';

			if (Flags & L4MORE)
				*(Output++) = 'M';
	
			if (Flags & L4COMP)
				*(Output++) = 'C';

			MsgLen = MsgLen - (19 + sizeof(void *));

			if (MsgLen < 0 || MsgLen > 257)
				return Output;				// Duff

			while (MsgLen--)
			{
				C = *(ptr++);

				// Convert to printable

				C &= 0x7F;

				if (C == 13 || C == 10 || C > 31)
					*(ptr1++) = C;	
			}

			len = ptr1 - Infofield;
	
			Output[0] = ':';
			Output[1] = 13;
			memcpy(&Output[2], Infofield, len);
			Output += (len + 2);
		}
		
		return Output;

	case L4IACK:

		Output += sprintf((char *)Output, " <INFO ACK R%d> ", RXNO);
	
		if (Flags & L4BUSY)
			*(Output++) = 'B';

		if (Flags & L4NAK)
			*(Output++) = 'N';

		if (Flags & L4MORE)
			*(Output++) = 'M';

		return Output;


	case 0:

		//	OPcode zero is used for several things

		if (Index == 0x0c && ID == 0x0c)	// IP	
		{
			*(Output++) = 13;
			*(Output++) = ' ';
			Output = DISPLAYIPDATAGRAM((IPMSG *)ptr, Output, MsgLen - (int)(19 + sizeof(void *)));
			return Output;
		}
	
		if (Index == 0 && ID == 1)			// NRR	
		{
			Output += sprintf((char *)Output, " <Record Route>\r");

			MsgLen -= 23;

			while (MsgLen > 6)
			{
				Dest[ConvFromAX25(ptr, Dest)] = 0;

				if (ptr[7] & 0x80)
					Output += sprintf((char *)Output, "%s* ", Dest);
				else
					Output += sprintf((char *)Output, "%s ", Dest);

				ptr +=8;
				MsgLen -= 8;
			}

			return Output;
		}

		if (Index == 0x0F)			// NCMP	
		{
			Output += sprintf((char *)Output, " NCMP Type %s Code %x", NCMPTypes[TXNO & 15], RXNO);
			
			MsgLen = MsgLen - (19 + sizeof(void *));

			if (MsgLen < 0 || MsgLen > 257)
				return Output;// Duff

			return Output;
		}
	}

	Output += sprintf((char *)Output, " <???\?>");
	return Output;
}

/*
	
	PUBLIC	L3IP
L3IP:
;
;	TCP/IP DATAGRAM
;
	mov	EBX,OFFSET IP_MSG
	call	NORMSTR
;
	INC	ESI			; NOW POINTING TO IP HEADER

*/
// linbpq-flexnet: verbose IP decode for the monitor (v2.11.6).
//
// Upstream printed only "src>dst LEN:n [DF] [MF] TCP Src a Dest b" (or ICMP
// echo). This keeps those tokens, in the same order, and adds TTL/ID, TCP
// flags/seq/ack/window/payload size, UDP ports, ICMP type/code/id/seq, the
// protocol number for anything else, and a printable preview of the payload
// carried in this frame.
//
// Avail = bytes of the datagram actually present in this frame (callers
// derive it from msg->LENGTH). Every field read is checked against it, so a
// truncated or bogus frame can never make us read past the buffer.

#define MON_IP_PREVIEW 64		// max payload bytes shown per frame
#define MON_IP_FIELD 128		// max chars one MonPut() call may write

// The DecodeFrame API passes no output size. Every piece here is bounded:
// each MonPut() writes < MON_IP_FIELD chars and the whole IP decode stays
// under 300 (worst case pinned by tools/unit/test_ip_decode.c), well inside
// the callers' buffers (telnet monitor: 1024).

static UCHAR * MonPut(UCHAR * Output, const char * Fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, Fmt);
	n = vsnprintf((char *)Output, MON_IP_FIELD, Fmt, ap);
	va_end(ap);

	if (n < 0)
		n = 0;
	else if (n >= MON_IP_FIELD)
		n = MON_IP_FIELD - 1;

	return Output + n;
}

static UCHAR * MonIPPreview(const UCHAR * Data, int Present, int Total, UCHAR * Output)
{
	// Printable ASCII only: the telnet monitor stream uses 0xFE/0xFF as
	// framing bytes, so nothing outside 0x20-0x7E may be copied through.

	// Present = payload bytes in this frame, Total = payload in the datagram.

	int i, n = (Present > MON_IP_PREVIEW) ? MON_IP_PREVIEW : Present;

	if (n <= 0)
		return Output;

	Output = MonPut(Output, "\r  Data: ");

	for (i = 0; i < n; i++)
		*(Output++) = (Data[i] >= 0x20 && Data[i] < 0x7f) ? Data[i] : '.';

	if (Total > n)
		Output = MonPut(Output, "...");

	return Output;
}

static const char * MonICMPName(int Type)
{
	switch (Type)
	{
	case 0: return "Echo Reply";
	case 3: return "Dest Unreachable";
	case 4: return "Source Quench";
	case 5: return "Redirect";
	case 8: return "Echo Request";
	case 11: return "Time Exceeded";
	case 12: return "Parameter Problem";
	case 13: return "Timestamp Request";
	case 14: return "Timestamp Reply";
	}
	return NULL;
}

static UCHAR * MonTCP(const UCHAR * T, int Avail, int SegLen, UCHAR * Output)
{
	PTCPMSG TCP = (PTCPMSG)T;
	int Flags, Off, DataLen;

	Output = MonPut(Output, "TCP Src %d Dest %d ", ntohs(TCP->SOURCEPORT), ntohs(TCP->DESTPORT));

	if (Avail < 20)
		return MonPut(Output, "(truncated)");

	Flags = TCP->TCPFLAGS & 0x3f;
	Off = (TCP->TCPCONTROL >> 4) * 4;

	Output = MonPut(Output, "Flags:");

	if (Flags == 0)
		Output = MonPut(Output, "none");
	else
		Output = MonPut(Output, "%s%s%s%s%s%s",
			(Flags & 0x02) ? "SYN," : "", (Flags & 0x10) ? "ACK," : "",
			(Flags & 0x08) ? "PSH," : "", (Flags & 0x01) ? "FIN," : "",
			(Flags & 0x04) ? "RST," : "", (Flags & 0x20) ? "URG," : "");

	if (Output[-1] == ',')
		Output--;

	Output = MonPut(Output, " Seq:%u", (unsigned int)ntohl(TCP->SEQNUM));

	if (Flags & 0x10)
		Output = MonPut(Output, " Ack:%u", (unsigned int)ntohl(TCP->ACKNUM));

	Output = MonPut(Output, " Win:%d", ntohs(TCP->WINDOW));

	if (Off < 20 || Off > SegLen)
		return MonPut(Output, " (bad offset %d)", Off);

	DataLen = SegLen - Off;
	Output = MonPut(Output, " Data:%d", DataLen);

	if (Off < Avail)
		Output = MonIPPreview(T + Off, (Avail - Off < DataLen) ? Avail - Off : DataLen, DataLen, Output);

	return Output;
}

static UCHAR * MonUDP(const UCHAR * U, int Avail, int SegLen, UCHAR * Output)
{
	PUDPMSG UDP = (PUDPMSG)U;

	if (Avail < 8)
		return MonPut(Output, "UDP (truncated)");

	Output = MonPut(Output, "UDP Src %d Dest %d Data:%d",
		ntohs(UDP->SOURCEPORT), ntohs(UDP->DESTPORT), SegLen - 8);

	return MonIPPreview(U + 8, ((Avail < SegLen) ? Avail : SegLen) - 8, SegLen - 8, Output);
}

static UCHAR * MonICMP(const UCHAR * I, int Avail, int SegLen, UCHAR * Output)
{
	PICMPMSG ICMP = (PICMPMSG)I;
	const char * Name;

	if (Avail < 4)
		return MonPut(Output, "ICMP (truncated)");

	Name = MonICMPName(ICMP->ICMPTYPE);

	if (Name)
		Output = MonPut(Output, "ICMP %s ", Name);
	else
		Output = MonPut(Output, "ICMP Type:%d ", ICMP->ICMPTYPE);

	if (ICMP->ICMPCODE || (ICMP->ICMPTYPE != 0 && ICMP->ICMPTYPE != 8))
		Output = MonPut(Output, "Code:%d ", ICMP->ICMPCODE);

	if ((ICMP->ICMPTYPE == 0 || ICMP->ICMPTYPE == 8) && Avail >= 8)
		Output = MonPut(Output, "ID:%d Seq:%d Data:%d",
			ntohs(ICMP->ICMPID), ntohs(ICMP->ICMPSEQUENCE), SegLen - 8);

	return Output;
}

UCHAR * DISPLAYIPDATAGRAM(IPMSG * IP, UCHAR * Output, int Avail)
{
	const UCHAR * Raw = (const UCHAR *)IP;
	UCHAR * ptr;
	USHORT FRAGWORD;
	int IHL, TotLen, SegLen, SegAvail;

	if (Avail < 20)
		return MonPut(Output, "<IP datagram too short: %d bytes>", Avail);

	ptr = (UCHAR *)&IP->IPSOURCE;
	Output = MonPut(Output, "%d.%d.%d.%d>", ptr[0], ptr[1], ptr[2], ptr[3]);

	ptr = (UCHAR *)&IP->IPDEST;
	TotLen = ntohs(IP->IPLENGTH);
	Output = MonPut(Output, "%d.%d.%d.%d LEN:%d TTL:%d ID:%d ", ptr[0], ptr[1], ptr[2], ptr[3],
		TotLen, IP->IPTTL, ntohs(IP->IPID));

	FRAGWORD = ntohs(IP->FRAGWORD);

	if (FRAGWORD)
	{
		//Bit 1: (DF) 0 = May Fragment,  1 = Don't Fragment.
		//Bit 2: (MF) 0 = Last Fragment, 1 = More Fragments.
		//Fragment Offset:  13 bits

		if (FRAGWORD & (1 << 14))
			Output = MonPut(Output, "DF ");

		if (FRAGWORD & (1 << 13))
			Output = MonPut(Output, "MF ");

		FRAGWORD &= 0x1fff;

		if (FRAGWORD)
			return MonPut(Output, "Offset %d Proto:%d", FRAGWORD * 8, IP->IPPROTOCOL);
	}

	IHL = (IP->VERLEN & 0x0f) * 4;

	if ((IP->VERLEN >> 4) != 4 || IHL < 20 || IHL > TotLen)
		return MonPut(Output, "(bad header: ver %d ihl %d)", IP->VERLEN >> 4, IHL);

	SegLen = TotLen - IHL;
	SegAvail = ((Avail < TotLen) ? Avail : TotLen) - IHL;

	if (SegAvail < 0)
		SegAvail = 0;

	switch (IP->IPPROTOCOL)
	{
	case 6:
		return MonTCP(Raw + IHL, SegAvail, SegLen, Output);

	case 17:
		return MonUDP(Raw + IHL, SegAvail, SegLen, Output);

	case 1:
		return MonICMP(Raw + IHL, SegAvail, SegLen, Output);
	}

	return MonPut(Output, "Proto:%d Data:%d", IP->IPPROTOCOL, SegLen);
}



char * DISPLAYARPDATAGRAM(UCHAR * Datagram, UCHAR * Output)
{
	UCHAR * ptr = Datagram;
	char Dest[10];
	
	if (ptr[7] == 1)		// Request
		return Output + sprintf((char *)Output, " ARP Request who has %d.%d.%d.%d? Tell %d.%d.%d.%d",
			ptr[26], ptr[27], ptr[28], ptr[29], ptr[15], ptr[16], ptr[17], ptr[18]);

	// Response

	Dest[ConvFromAX25(&ptr[8], Dest)] = 0;

	return Output + sprintf((char *)Output, " ARP Reply %d.%d.%d.%d is at %s Tell %d.%d.%d.%d",
			ptr[15], ptr[16], ptr[17], ptr[18], Dest, ptr[26], ptr[27], ptr[28], ptr[29]);

}

char Lastpacketlog[256];

int PacketLogDelay = 30000;

extern BPQVECSTRUC * FILEMONVECTOR;
extern UCHAR LogDirectory[260];

void WritePacketLogThread(void * param)
{
	char FN[256];
	time_t T;
	struct tm * tm;
	FILE * Handle;
	int MsgLen;
	MESSAGE * MSG;
	MESSAGE * Q;
	char buffer[2048];
	char * ptr;

	while(1)
	{
		BOOL SaveMTX = MTX;
		BOOL SaveMCOM = MCOM;
		BOOL SaveMUI = MUIONLY;

		Sleep(PacketLogDelay);

		// Get chain of queued packets under semaphore

		GetSemaphore(&Semaphore, 101);

		Q = FILEMONVECTOR->HOSTTRACEQ;
		FILEMONVECTOR->HOSTTRACEQ = 0;

		FreeSemaphore(&Semaphore);
		
		if (Q == 0)
			continue;

		// Open log file and write decoded packets

		T = NOW;
		tm = gmtime(&T);

		if (LogDirectory[0] == 0)
		{
			strcpy(FN, "logs/PacketLog");
		}
		else
		{
			strcpy(FN, LogDirectory);
			strcat(FN, "/");
			strcat(FN, "logs/PacketLog");
		}

		sprintf(&FN[strlen(FN)], "_%04d%02d%02d.log", tm->tm_year +1900, tm->tm_mon+1, tm->tm_mday);

		Handle = fopen(FN, "ab");

		if (Handle == NULL)
			return;

		while (Q)
		{
			MSG = Q;
			Q = MSG->CHAIN;			// get first

			IntSetTraceOptionsEx(MMASK, 1, 1, 0);
			MsgLen = IntDecodeFrame(MSG, buffer, MSG->Timestamp, 0xffffffffffffffff, FALSE, FALSE);
			IntSetTraceOptionsEx(MMASK, SaveMTX, SaveMCOM, SaveMUI);

			// lines are CR terminated. Replace with LF

			ptr = buffer;

			while (ptr = strchr(ptr, 0x0d))
				*ptr = 0x0a;

//			buffer[MsgLen++] = 0x0a;		// Add lf

			fwrite(buffer , 1, MsgLen, Handle);

			GetSemaphore(&Semaphore, 101);
			ReleaseBuffer(MSG);
			FreeSemaphore(&Semaphore);
		}
		fclose(Handle);
	}

	return;
}

