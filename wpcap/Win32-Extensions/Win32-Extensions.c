/*
 * Copyright (c) 1999 - 2002
 *	Politecnico di Torino.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that: (1) source code distributions
 * retain the above copyright notice and this paragraph in its entirety, (2)
 * distributions including binary code include the above copyright notice and
 * this paragraph in its entirety in the documentation or other materials
 * provided with the distribution, and (3) all advertising materials mentioning
 * features or use of this software display the following acknowledgement:
 * ``This product includes software developed by the Politecnico
 * di Torino, and its contributors.'' Neither the name of
 * the University nor the names of its contributors may be used to endorse
 * or promote products derived from this software without specific prior
 * written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#include "pcap-int.h"
#include <packet32.h>

HANDLE
pcap_getevent(pcap_t *p)
{
	if (p->adapter==NULL)
	{
		sprintf(p->errbuf, "The read event cannot be retrieved while reading from a file");
		return NULL;
	}	

	return PacketGetReadEvent(p->adapter);

}

pcap_send_queue* 
pcap_sendqueue_alloc(u_int memsize)
{

	pcap_send_queue *tqueue;

	/* Allocate the queue */
	tqueue = (pcap_send_queue*)malloc(sizeof(pcap_send_queue));
	if(tqueue == NULL){
		return NULL;
	}

	/* Allocate the buffer */
	tqueue->buffer = (char*)malloc(memsize);
	if(tqueue->buffer == NULL){
		free(tqueue);
		return NULL;
	}

	tqueue->maxlen = memsize;
	tqueue->len = 0;

	return tqueue;
}

void 
pcap_sendqueue_destroy(pcap_send_queue* queue)
{
	free(queue->buffer);
	free(queue);
}

int 
pcap_sendqueue_queue(pcap_send_queue* queue, const struct pcap_pkthdr *pkt_header, const u_char *pkt_data)
{

	if(queue->len + sizeof(struct pcap_pkthdr) + pkt_header->caplen > queue->maxlen){
		return -1;
	}

	/* Copy the pcap_pkthdr header*/
	memcpy(queue->buffer + queue->len, pkt_header, sizeof(struct pcap_pkthdr));
	queue->len += sizeof(struct pcap_pkthdr);

	/* copy the packet */
	memcpy(queue->buffer + queue->len, pkt_data, pkt_header->caplen);
	queue->len += pkt_header->caplen;

	return 0;
}

u_int 
pcap_sendqueue_transmit(pcap_t *p, pcap_send_queue* queue, int sync){

	u_int res;
	DWORD error;
	int errlen;

	res = PacketSendPackets(p->adapter,
		queue->buffer,
		queue->len,
		(BOOLEAN)sync);

	if(res != queue->len){
		error = GetLastError();
		FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM,NULL,error,0,p->errbuf,PCAP_ERRBUF_SIZE,NULL);
		/*
		* "FormatMessage()" "helpfully" sticks CR/LF at the end of
		* the message.  Get rid of it.
		*/
		errlen = strlen(p->errbuf);
		if (errlen >= 2) {
			p->errbuf[errlen - 1] = '\0';
			p->errbuf[errlen - 2] = '\0';
		}
		snprintf(p->errbuf, PCAP_ERRBUF_SIZE, "Error opening adapter: %s", p->errbuf);
	}

	return res;
}


int 
pcap_read_ex(pcap_t *p, struct pcap_pkthdr **pkt_header, u_char **pkt_data)
{
	/* Check the capture type */
	if (p->adapter!=NULL)
	{
		/* We are on a live capture */
		int cc;
		int n = 0;
		register u_char *bp, *ep;
		
		cc = p->cc;
		if (p->cc == 0) 
		{
			/* capture the packets */
			if(PacketReceivePacket(p->adapter,p->Packet,TRUE)==FALSE)
			{
				sprintf(p->errbuf, "read error: PacketReceivePacket failed");
				return (-1);
			}
			
			cc = p->Packet->ulBytesReceived;
			
			bp = p->Packet->Buffer;
		} 
		else
			bp = p->bp;
		
		/*
		 * Loop through each packet.
		 */
		ep = bp + cc;
		if (bp < ep) 
		{
			register int caplen, hdrlen;
			caplen = ((struct bpf_hdr *)bp)->bh_caplen;
			hdrlen = ((struct bpf_hdr *)bp)->bh_hdrlen;
			
			/*
			 * XXX A bpf_hdr matches a pcap_pkthdr.
			 */
			*pkt_header = (struct pcap_pkthdr*)bp;
			*pkt_data = bp + hdrlen;
			bp += BPF_WORDALIGN(caplen + hdrlen);
			
			p->bp = bp;
			p->cc = ep - bp;
			return (1);
		}
		else{
			p->cc = 0;
			return (0);
		}
	}	
	else
	{
		/* We are on an offline capture */
		struct bpf_insn *fcode = p->fcode.bf_insns;
		int status;
		int n = 0;
		
		struct pcap_pkthdr *h=(struct pcap_pkthdr*)(p->buffer+p->bufsize-sizeof(struct pcap_pkthdr));
		
		while (1)
		{
			status = sf_next_packet(p, h, p->buffer, p->bufsize);
			if (status==1)
				/* EOF */
				return (-2);
			if (status==-1)
				/* Error */
				return (-1);
			
			if (fcode == NULL ||
				bpf_filter(fcode, p->buffer, h->len, h->caplen)) 
			{
				*pkt_header = h;
				*pkt_data = p->buffer;
				return (1);
			}			
			
		}
	}
}


int
pcap_setuserbuffer(pcap_t *p, int size)

{
	unsigned char *new_buff;

	if (!p->adapter) {
		sprintf(p->errbuf,"Impossible to set user buffer while reading from a file");
		return -1;
	}

	if (size<=0) {
		/* Bogus parameter */
		sprintf(p->errbuf,"Error: invalid size %d",size);
		return -1;
	}

	/* Allocate the buffer */
	new_buff=(unsigned char*)malloc(sizeof(char)*size);

	if (!new_buff) {
		sprintf(p->errbuf,"Error: not enough memory");
		return -1;
	}

	free(p->buffer);
	
	p->buffer=new_buff;
	p->bufsize=size;

	/* Associate the buffer with the capture packet */
	PacketInitPacket(p->Packet,(BYTE*)p->buffer,p->bufsize);

	return 0;

}
