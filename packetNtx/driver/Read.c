/*
 * Copyright (c) 1999, 2000
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

#include <stdarg.h>
#include "ntddk.h"
#include <ntiologc.h>
#include <ndis.h>
#include "debug.h"
#include "packet.h"
#include "win_bpf.h"

#include "tme.h"
#include "time_calls.h"


//-------------------------------------------------------------------

UINT GetBuffOccupation(POPEN_INSTANCE Open)
{
	UINT Occupation;

	NdisAcquireSpinLock( &Open->BufLock );
	
	if(Open->Btail >= Open->Bhead) Occupation = Open->Btail-Open->Bhead;
	else Occupation = Open->BLastByte-Open->Bhead+Open->Btail;

	NdisReleaseSpinLock( &Open->BufLock );

	return Occupation;
}

//-------------------------------------------------------------------

void PacketMoveMem(PVOID Destination, PVOID Source, ULONG Length, UINT	 *Bhead)
{
ULONG WordLength;
UINT n,i,NBlocks;

	WordLength=Length>>2;
	NBlocks=WordLength>>8;
	
	for(n=0;n<NBlocks;n++){
		for(i=0;i<256;i++){
			*((PULONG)Destination)++=*((PULONG)Source)++;
		}
	*Bhead+=1024;
	}

	n=WordLength-(NBlocks<<8);
	for(i=0;i<n;i++){
		*((PULONG)Destination)++=*((PULONG)Source)++;
	}
	*Bhead+=n<<2;
	
	n=Length-(WordLength<<2);
	for(i=0;i<n;i++){
		*((PUCHAR)Destination)++=*((PUCHAR)Source)++;
	}
	*Bhead+=n;
}

//-------------------------------------------------------------------

NTSTATUS NPF_Read(IN PDEVICE_OBJECT DeviceObject,IN PIRP Irp)
{
    POPEN_INSTANCE      Open;
    PIO_STACK_LOCATION  IrpSp;
    PUCHAR				packp;
	UINT				i;
	ULONG				Input_Buffer_Length;
	UINT				Thead;
	UINT				Ttail;
	UINT				TLastByte;
	PUCHAR				CurrBuff;
	UINT				cplen;
	UINT				CpStart;
	LARGE_INTEGER		CapTime;
	LARGE_INTEGER		TimeFreq;
	struct bpf_hdr		*header;
	KIRQL				Irql;
	PUCHAR				UserPointer;
	ULONG				bytecopy;

	IF_LOUD(DbgPrint("NPF: Read\n");)
		
	IrpSp = IoGetCurrentIrpStackLocation(Irp);
    Open=IrpSp->FileObject->FsContext;
	
	if( Open->Bound == FALSE ){
		// The Network adapter was removed.
		EXIT_FAILURE(0);
	}
	
	if( Open->mode & MODE_DUMP && Open->DumpFileHandle != NULL ){
		// this instance is in dump mode, but the dump file has still not been opened
		EXIT_FAILURE(0);
	}

	//See if the buffer is full enough to be copied
	if( GetBuffOccupation(Open) <= Open->MinToCopy || Open->mode & MODE_DUMP )
	{
		//wait until some packets arrive or the timeout expires		
		if(Open->TimeOut.QuadPart != (LONGLONG)IMMEDIATE)
			KeWaitForSingleObject(Open->ReadEvent,
				UserRequest,
				KernelMode,
				TRUE,
				(Open->TimeOut.QuadPart == (LONGLONG)0)? NULL: &(Open->TimeOut));

		KeClearEvent(Open->ReadEvent);
		
		if(Open->mode & MODE_STAT){   //this capture instance is in statistics mode
			CurrBuff=(PUCHAR)MmGetSystemAddressForMdl(Irp->MdlAddress);
			
			//get the timestamp
//OLD		CapTime=KeQueryPerformanceCounter(&TimeFreq); 

			//fill the bpf header for this packet
//OLD		CapTime.QuadPart+=Open->StartTime.QuadPart;
			header=(struct bpf_hdr*)CurrBuff;
//OLD		header->bh_tstamp.tv_usec=(long)((CapTime.QuadPart%TimeFreq.QuadPart*1000000)/TimeFreq.QuadPart);
//OLD		header->bh_tstamp.tv_sec=(long)(CapTime.QuadPart/TimeFreq.QuadPart);
			GET_TIME(&header->bh_tstamp,&Open->start_time);

			if(Open->mode & MODE_DUMP){
				*(LONGLONG*)(CurrBuff+sizeof(struct bpf_hdr)+16)=Open->DumpOffset.QuadPart;
				header->bh_caplen=24;
				header->bh_datalen=24;
				Irp->IoStatus.Information = 24 + sizeof(struct bpf_hdr);
			}
			else{
				header->bh_caplen=16;
				header->bh_datalen=16;
				header->bh_hdrlen=sizeof(struct bpf_hdr);
				Irp->IoStatus.Information = 16 + sizeof(struct bpf_hdr);
			}

			*(LONGLONG*)(CurrBuff+sizeof(struct bpf_hdr))=Open->Npackets.QuadPart;
			*(LONGLONG*)(CurrBuff+sizeof(struct bpf_hdr)+8)=Open->Nbytes.QuadPart;
			
			//reset the countetrs
			NdisAcquireSpinLock( &Open->CountersLock );
			Open->Npackets.QuadPart=0;
			Open->Nbytes.QuadPart=0;
			NdisReleaseSpinLock( &Open->CountersLock );
			
			Irp->IoStatus.Status = STATUS_SUCCESS;
			IoCompleteRequest(Irp, IO_NO_INCREMENT);
			
			return STATUS_SUCCESS;
		}
		
		if(Open->mode==MODE_MON)   //this capture instance is in monitor mode
		{   
			PTME_DATA data;
			ULONG cnt;
			ULONG block_size;
			PUCHAR tmp;

			UserPointer=MmGetSystemAddressForMdl(Irp->MdlAddress);
			
			if ((!IS_VALIDATED(Open->tme.validated_blocks,Open->tme.active_read))||(IrpSp->Parameters.Read.Length<sizeof(struct bpf_hdr)))
			{	
				EXIT_FAILURE(0);
			}
			
			header=(struct bpf_hdr*)UserPointer;
	
			GET_TIME(&header->bh_tstamp,&Open->start_time);

			
			header->bh_hdrlen=sizeof(struct bpf_hdr);
			

			//moves user memory pointer
			UserPointer+=sizeof(struct bpf_hdr);
			
			//calculus of data to be copied
			//if the user buffer is smaller than data to be copied,
			//only some data will be copied
			data=&Open->tme.block_data[Open->tme.active_read];

			if (data->last_read.tv_sec!=0)
				data->last_read=header->bh_tstamp;
			

			bytecopy=data->block_size*data->filled_blocks;
			
			if ((IrpSp->Parameters.Read.Length-sizeof(struct bpf_hdr))<bytecopy)
				bytecopy=(IrpSp->Parameters.Read.Length-sizeof(struct bpf_hdr))/ data->block_size;
			else 
				bytecopy=data->filled_blocks;

			tmp=data->shared_memory_base_address;
			block_size=data->block_size;
			
			for (cnt=0;cnt<bytecopy;cnt++)
			{
				NdisAcquireSpinLock(&Open->machine_lock);
				RtlCopyMemory(UserPointer,tmp,block_size);
				NdisReleaseSpinLock(&Open->machine_lock);
				tmp+=block_size;
				UserPointer+=block_size;
			}
						
			bytecopy*=block_size;

			header->bh_caplen=bytecopy;
			header->bh_datalen=header->bh_caplen;

			EXIT_SUCCESS(bytecopy+sizeof(struct bpf_hdr));
		}

		if (Open->Bhead == Open->Btail || Open->mode & MODE_DUMP)
			// The timeout has expired, but the buffer is still empty (or the packets must be written to file).
			// We must awake the application, returning an empty buffer.
		{
			EXIT_SUCCESS(0);
		}
				
	}

	//
	// The buffer if full enough to be copied,
	//
	NdisAcquireSpinLock( &Open->BufLock );
	
	Thead=Open->Bhead;
	Ttail=Open->Btail;
	TLastByte=Open->BLastByte;

	//get the address of the buffer
	CurrBuff=Open->Buffer;
	
	NdisReleaseSpinLock( &Open->BufLock );
	
	Input_Buffer_Length=IrpSp->Parameters.Read.Length;
	packp=(PUCHAR)MmGetMdlVirtualAddress(Irp->MdlAddress);
	

	//
	//fill the application buffer
	//
	if(Ttail>Thead){ 	//first of all see if it we can copy all the buffer in one time
		if((Ttail-Thead)<Input_Buffer_Length){
			KeResetEvent(Open->ReadEvent);

			PacketMoveMem(packp,CurrBuff+Thead,Ttail-Thead,&(Open->Bhead));
			EXIT_SUCCESS(Ttail-Thead);
		}
	}
	else if((TLastByte-Thead)<Input_Buffer_Length){
		PacketMoveMem(packp,CurrBuff+Thead,TLastByte-Thead,&(Open->Bhead));

		NdisAcquireSpinLock( &Open->BufLock );
		
		Open->BLastByte=Open->Btail;
		Open->Bhead=0;

		NdisReleaseSpinLock( &Open->BufLock );
		
		EXIT_SUCCESS(TLastByte-Thead);
	}
	
	//the buffer must be scannned to determine the number of bytes to copy
	CpStart=Thead;
	i=0;
	while(TRUE){
		if(Thead==Ttail)break;

		if(Thead==TLastByte){
			// Copy the portion between thead and TLastByte
			PacketMoveMem(packp,CurrBuff+CpStart,Thead-CpStart,&(Open->Bhead));
			packp+=(Thead-CpStart);

			NdisAcquireSpinLock( &Open->BufLock );
			
			Open->BLastByte=Open->Btail;
 			Open->Bhead=0;
			
			NdisReleaseSpinLock( &Open->BufLock );

			Thead=0;
			CpStart=0;
		}
		cplen=((struct bpf_hdr*)(CurrBuff+Thead))->bh_caplen+sizeof(struct bpf_hdr);

		if((i+cplen > Input_Buffer_Length)){//no more space in the application's buffer
			PacketMoveMem(packp,CurrBuff+CpStart,Thead-CpStart,&(Open->Bhead));

			EXIT_SUCCESS(i);
		}
		cplen=Packet_WORDALIGN(cplen);
		i+=cplen;
		Thead+=cplen;
	}
	

	KeResetEvent(Open->ReadEvent);
	
	PacketMoveMem(packp,CurrBuff+CpStart,Thead-CpStart,&(Open->Bhead));
	
	Open->Bhead=Thead;
	
	
	EXIT_SUCCESS(i);	
}

//-------------------------------------------------------------------

NDIS_STATUS NPF_tap (IN NDIS_HANDLE ProtocolBindingContext,IN NDIS_HANDLE MacReceiveContext,
                        IN PVOID HeaderBuffer,IN UINT HeaderBufferSize,IN PVOID LookAheadBuffer,
                        IN UINT LookaheadBufferSize,IN UINT PacketSize)
{
    POPEN_INSTANCE      Open;
    PNDIS_PACKET        pPacketb;
    ULONG               SizeToTransfer;
    NDIS_STATUS         Status;
    UINT                BytesTransfered;
    ULONG               BufferLength;
    PMDL                pMdl;
	LARGE_INTEGER		CapTime;
	LARGE_INTEGER		TimeFreq;
	struct bpf_hdr		*header;
	PUCHAR				CurrBuff;
	UINT				Thead;
	UINT				Ttail;
	UINT				TLastByte;
	UINT				fres;
	UINT				maxbufspace;
	USHORT				NPFHdrSize;
	UINT				BufOccupation;

    IF_LOUD(DbgPrint("NPF: tap\n");)

	Open= (POPEN_INSTANCE)ProtocolBindingContext;

	Open->Received++;		//number of packets received by filter ++

	NdisAcquireSpinLock(&Open->machine_lock);
	
	//
	//Check if the lookahead buffer follows the mac header.
	//If the data follow the header (i.e. there is only a buffer) a normal bpf_filter() is
	//executed on the packet.
	//Otherwise if there are 2 separate buffers (this could be the case of LAN emulation or
	//things like this) bpf_filter_with_2_buffers() is executed.
	//
	if((UINT)LookAheadBuffer-(UINT)HeaderBuffer != HeaderBufferSize)
		fres=bpf_filter_with_2_buffers((struct bpf_insn*)(Open->bpfprogram),
									   HeaderBuffer,
									   LookAheadBuffer,
									   HeaderBufferSize,
									   PacketSize+HeaderBufferSize,
									   LookaheadBufferSize+HeaderBufferSize,
									   &Open->mem_ex,
									   &Open->tme,
									   &Open->start_time);
	
	
	else 
		if(Open->Filter != NULL)
		{
			if (Open->bpfprogram!=NULL)
			{
				fres=Open->Filter->Function(HeaderBuffer,
									PacketSize+HeaderBufferSize,
									LookaheadBufferSize+HeaderBufferSize);
		
				// Restore the stack. 
				// I ignore the reason, but this instruction is needed only at kernel level
				_asm add esp,12		
			}
			else
				fres = -1;
		}
		else
 			fres=bpf_filter((struct bpf_insn*)(Open->bpfprogram),
 		                HeaderBuffer,
 						PacketSize+HeaderBufferSize,
						LookaheadBufferSize+HeaderBufferSize,
						&Open->mem_ex,
						&Open->tme,
						&Open->start_time);

	NdisReleaseSpinLock(&Open->machine_lock);
	
	if(Open->mode==MODE_MON)
	// we are in monitor mode
	{
		if (fres==1) 
			KeSetEvent(Open->ReadEvent,0,FALSE);
		return NDIS_STATUS_NOT_ACCEPTED;

	}

	if(fres==0)return NDIS_STATUS_NOT_ACCEPTED; //packet not accepted by the filter

	//if the filter returns -1 the whole packet must be accepted
	if(fres==-1 || fres > PacketSize+HeaderBufferSize)fres=PacketSize+HeaderBufferSize; 

	if(Open->mode & MODE_STAT){
	// we are in statistics mode
		NdisAcquireSpinLock( &Open->CountersLock );

		Open->Npackets.QuadPart++;
		
		if(PacketSize+HeaderBufferSize<60)
			Open->Nbytes.QuadPart+=60;
		else
			Open->Nbytes.QuadPart+=PacketSize+HeaderBufferSize;
		// add preamble+SFD+FCS to the packet
		// these values must be considered because are not part of the packet received from NDIS
		Open->Nbytes.QuadPart+=12;

		NdisReleaseSpinLock( &Open->CountersLock );
		
		if(!(Open->mode & MODE_DUMP)){
 			return NDIS_STATUS_NOT_ACCEPTED;
		}
	}

	if(Open->BufSize==0)return NDIS_STATUS_NOT_ACCEPTED;
	
	// Calculate the correct size for the header associated with the packet
 	NPFHdrSize=(Open->mode==MODE_CAPT)? sizeof(struct bpf_hdr): sizeof(struct sf_pkthdr);
 
	NdisAcquireSpinLock( &Open->BufLock );

	Thead=Open->Bhead;
	Ttail=Open->Btail;
	TLastByte = Open->BLastByte;
	BufOccupation = GetBuffOccupation(Open);

	NdisReleaseSpinLock( &Open->BufLock );
	
	maxbufspace=Packet_WORDALIGN(fres+NPFHdrSize);

	
	if(Open->BufSize <= BufOccupation + maxbufspace)
	{
		Open->Dropped++;
		
		return NDIS_STATUS_NOT_ACCEPTED;
	}


	if(Ttail+maxbufspace >= Open->BufSize){
		if(Thead<=maxbufspace)
		{
			//the buffer is full: the packet is lost
			Open->Dropped++;
			return NDIS_STATUS_NOT_ACCEPTED;
		}
		else{
			Ttail=0;
		}
	}
	
	CurrBuff=Open->Buffer+Ttail;

	if(LookaheadBufferSize != PacketSize || (UINT)LookAheadBuffer-(UINT)HeaderBuffer != HeaderBufferSize)
	{
		//  Allocate an MDL to map the portion of the buffer following the header
		pMdl=IoAllocateMdl(CurrBuff+HeaderBufferSize+LookaheadBufferSize+NPFHdrSize,
			maxbufspace,
			FALSE,
			FALSE,
			NULL);

		if (pMdl == NULL)
		{
			// Unable to map the memory: packet lost
			IF_LOUD(DbgPrint("NPF: Read-Failed to allocate Mdl\n");)
				Open->Dropped++;
			return NDIS_STATUS_NOT_ACCEPTED;
		}
		MmBuildMdlForNonPagedPool(pMdl);
		
		//allocate the packet from NDIS
		NdisAllocatePacket(&Status, &pPacketb, Open->PacketPool);
		if (Status != NDIS_STATUS_SUCCESS)
		{
			IF_LOUD(DbgPrint("NPF: Tap - No free packets\n");)
			IoFreeMdl(pMdl);
			Open->Dropped++;
			return NDIS_STATUS_NOT_ACCEPTED;
		}
		//link the buffer to the packet
		NdisChainBufferAtFront(pPacketb,pMdl);
		
		BufferLength=fres-HeaderBufferSize;
		//Find out how much to transfer
		SizeToTransfer = (PacketSize < BufferLength) ? PacketSize : BufferLength;
		
		//copy the ethernet header into buffer
		NdisMoveMappedMemory((CurrBuff)+NPFHdrSize,HeaderBuffer,HeaderBufferSize);
		
		//Copy the look ahead buffer
		if(LookaheadBufferSize)
		{
			NdisMoveMappedMemory((CurrBuff) + NPFHdrSize + HeaderBufferSize,
				LookAheadBuffer, 
				(SizeToTransfer < LookaheadBufferSize)?	SizeToTransfer : LookaheadBufferSize );
			
			SizeToTransfer = (SizeToTransfer > LookaheadBufferSize)?
				SizeToTransfer - LookaheadBufferSize : 0;
		}
		
		Open->TransferMdl=pMdl;
		
		if(SizeToTransfer)
		{
			//Call the Mac to transfer the packet
			NdisTransferData(&Status,
				Open->AdapterHandle,
				MacReceiveContext,
				LookaheadBufferSize,
				SizeToTransfer,
				pPacketb,
				&BytesTransfered);
		}
		else{
			BytesTransfered = 0;
		}
		
	}
	else
	{
	// The whole packet is in the lookahead buffer, we can avoid the call to NdisTransferData.
	// This allows us to avoid the allocation of the MDL and the NDIS packet as well
	RtlCopyMemory((CurrBuff) + NPFHdrSize,
		HeaderBuffer,
		HeaderBufferSize + LookaheadBufferSize);

		Open->TransferMdl = NULL;
		Status = NDIS_STATUS_SUCCESS;
	}


	if (Status != NDIS_STATUS_FAILURE)
	{
		//
		// Build the header
		//
		
		CapTime=KeQueryPerformanceCounter(&TimeFreq); 

		if( fres > (BytesTransfered+HeaderBufferSize+LookaheadBufferSize) )
			fres = BytesTransfered+HeaderBufferSize+LookaheadBufferSize;

		//fill the bpf header for this packet
		CapTime.QuadPart+=Open->StartTime.QuadPart;
		header=(struct bpf_hdr*)CurrBuff;
		header->bh_tstamp.tv_usec=(long)((CapTime.QuadPart%TimeFreq.QuadPart*1000000)/TimeFreq.QuadPart);
		header->bh_tstamp.tv_sec=(long)(CapTime.QuadPart/TimeFreq.QuadPart);
		header->bh_caplen=fres;
		header->bh_datalen=PacketSize+HeaderBufferSize;
		if(Open->mode==MODE_CAPT){
			header->bh_hdrlen=NPFHdrSize;
			// Don't align if the packet goes to disk
			Ttail+=Packet_WORDALIGN(fres + NPFHdrSize);
		}
		else
			Ttail+=fres+NPFHdrSize;
		
		//update the buffer	
		if(Ttail>Thead)TLastByte=Ttail;

		NdisAcquireSpinLock( &Open->BufLock );
		
		Open->Btail=Ttail;
		Open->BLastByte=TLastByte;
		
		NdisReleaseSpinLock( &Open->BufLock );
	}

	if (Status != NDIS_STATUS_PENDING){

		if( Open->TransferMdl != NULL)
			// Complete the request and free the buffers
			NPF_TransferDataComplete(Open,pPacketb,Status,fres);
		else{
			// Unfreeze the consumer
			if(GetBuffOccupation(Open)>Open->MinToCopy){
				if(Open->mode & MODE_DUMP){
					NdisSetEvent(&Open->DumpEvent);
				}
				else
					KeSetEvent(Open->ReadEvent,0,FALSE);	
			}
			
		}
	}
	
	return NDIS_STATUS_SUCCESS;
	
}

//-------------------------------------------------------------------

VOID NPF_TransferDataComplete (IN NDIS_HANDLE ProtocolBindingContext,IN PNDIS_PACKET pPacket,
                                 IN NDIS_STATUS Status,IN UINT BytesTransfered)
{
    POPEN_INSTANCE      Open;

    IF_LOUD(DbgPrint("NPF: TransferDataComplete\n");)
    
	Open= (POPEN_INSTANCE)ProtocolBindingContext;

	IoFreeMdl(Open->TransferMdl);
	//recylcle the packet
	NdisReinitializePacket(pPacket);
	//Put the packet on the free queue
	NdisFreePacket(pPacket);
	// Unfreeze the consumer
	if(GetBuffOccupation(Open)>Open->MinToCopy){
 		if(Open->mode & MODE_DUMP){
 			NdisSetEvent(&Open->DumpEvent);
 		}
 		else
 			KeSetEvent(Open->ReadEvent,0,FALSE);	
	}
	return;
}

//-------------------------------------------------------------------

VOID NPF_ReceiveComplete(IN NDIS_HANDLE ProtocolBindingContext)
{
    IF_LOUD(DbgPrint("NPF: NPF_ReceiveComplete\n");)
    return;
}