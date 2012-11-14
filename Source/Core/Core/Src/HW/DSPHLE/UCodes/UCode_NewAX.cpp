// Copyright (C) 2003 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official Git repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#include "UCode_NewAX.h"
#include "UCode_AX_Voice.h"
#include "../../DSP.h"

// Useful macro to convert xxx_hi + xxx_lo to xxx for 32 bits.
#define HILO_TO_32(name) \
	((name##_hi << 16) | name##_lo)

#define MIXBUF_MAX_SAMPLES 16000			// 500ms of stereo audio

CUCode_NewAX::CUCode_NewAX(DSPHLE* dsp_hle, u32 crc)
	: IUCode(dsp_hle, crc)
	, m_cmdlist_size(0)
	, m_axthread(&SpawnAXThread, this)
{
	m_rMailHandler.PushMail(DSP_INIT);
	DSP::GenerateDSPInterruptFromDSPEmu(DSP::INT_DSP);
}

CUCode_NewAX::~CUCode_NewAX()
{
	m_cmdlist_size = (u16)-1;	// Special value to signal end
	NotifyAXThread();
	m_axthread.join();

	m_rMailHandler.Clear();
}

void CUCode_NewAX::SpawnAXThread(CUCode_NewAX* self)
{
	self->AXThread();
}

void CUCode_NewAX::AXThread()
{
	while (true)
	{
		{
			std::unique_lock<std::mutex> lk(m_cmdlist_mutex);
			while (m_cmdlist_size == 0)
				m_cmdlist_cv.wait(lk);
		}

		if (m_cmdlist_size == (u16)-1)	// End of thread signal
			break;

		m_processing.lock();
		HandleCommandList();
		m_cmdlist_size = 0;

		// Signal end of processing
		m_rMailHandler.PushMail(DSP_YIELD);
		DSP::GenerateDSPInterruptFromDSPEmu(DSP::INT_DSP);
		m_processing.unlock();
	}
}

void CUCode_NewAX::NotifyAXThread()
{
	std::unique_lock<std::mutex> lk(m_cmdlist_mutex);
	m_cmdlist_cv.notify_one();
}

void CUCode_NewAX::HandleCommandList()
{
	// Temp variables for addresses computation
	u16 addr_hi, addr_lo;
	u16 addr2_hi, addr2_lo;

	u32 pb_addr = 0;

	u32 curr_idx = 0;
	bool end = false;
	while (!end)
	{
		u16 cmd = m_cmdlist[curr_idx++];

		switch (cmd)
		{

			// A lot of these commands are unknown, or unused in this AX HLE.
			// We still need to skip their arguments using "curr_idx += N".

			case CMD_SETUP:
				addr_hi = m_cmdlist[curr_idx++];
				addr_lo = m_cmdlist[curr_idx++];
				SetupProcessing(HILO_TO_32(addr));
				break;

			case CMD_UNK_01: curr_idx += 5; break;

			case CMD_PB_ADDR:
				addr_hi = m_cmdlist[curr_idx++];
				addr_lo = m_cmdlist[curr_idx++];
				pb_addr = HILO_TO_32(addr);
				break;

			case CMD_PROCESS:
				ProcessPBList(pb_addr);
				break;

			case CMD_MIX_AUXA:
			case CMD_MIX_AUXB:
				// These two commands are handled almost the same internally.
				addr_hi = m_cmdlist[curr_idx++];
				addr_lo = m_cmdlist[curr_idx++];
				addr2_hi = m_cmdlist[curr_idx++];
				addr2_lo = m_cmdlist[curr_idx++];
				MixAUXSamples(cmd == CMD_MIX_AUXA, HILO_TO_32(addr), HILO_TO_32(addr2));
				break;

			case CMD_MIX_AUXB_NOWRITE:
				addr_hi = m_cmdlist[curr_idx++];
				addr_lo = m_cmdlist[curr_idx++];
				MixAUXSamples(false, 0, HILO_TO_32(addr));
				break;

			case CMD_SBUFFER_ADDR: curr_idx += 2; break;
			case CMD_UNK_08: curr_idx += 10; break;	// TODO: check
			case CMD_UNK_09: curr_idx += 2; break;
			case CMD_COMPRESSOR_TABLE_ADDR: curr_idx += 2; break;
			case CMD_UNK_0B: break; // TODO: check other versions
			case CMD_UNK_0C: break; // TODO: check other versions
			case CMD_UNK_0D: curr_idx += 2; break;

			case CMD_OUTPUT:
				// Skip the first address, it is used for surround audio
				// output, which we don't support yet.
				curr_idx += 2;

				addr_hi = m_cmdlist[curr_idx++];
				addr_lo = m_cmdlist[curr_idx++];
				OutputSamples(HILO_TO_32(addr));
				break;

			case CMD_END:
				end = true;
				break;

			case CMD_UNK_10: curr_idx += 4; break;
			case CMD_UNK_11: curr_idx += 2; break;
			case CMD_UNK_12: curr_idx += 1; break;
			case CMD_UNK_13: curr_idx += 12; break;

			default:
				ERROR_LOG(DSPHLE, "Unknown command in AX cmdlist: %04x", cmd);
				end = true;
				break;
		}
	}
}

// From old UCode_AX.cpp.
static void VoiceHacks(AXPB &pb)
{
	// get necessary values
	const u32 sampleEnd = (pb.audio_addr.end_addr_hi << 16) | pb.audio_addr.end_addr_lo;
	const u32 loopPos   = (pb.audio_addr.loop_addr_hi << 16) | pb.audio_addr.loop_addr_lo;
	// 		const u32 updaddr   = (u32)(pb.updates.data_hi << 16) | pb.updates.data_lo;
	// 		const u16 updpar    = HLEMemory_Read_U16(updaddr);
	// 		const u16 upddata   = HLEMemory_Read_U16(updaddr + 2);

	// =======================================================================================
	/* Fix problems introduced with the SSBM fix. Sometimes when a music stream ended sampleEnd
	would end up outside of bounds while the block was still playing resulting in noise 
	a strange noise. This should take care of that.
	*/
	if ((sampleEnd > (0x017fffff * 2) || loopPos > (0x017fffff * 2))) // ARAM bounds in nibbles
	{
		pb.running = 0;

		// also reset all values if it makes any difference
		pb.audio_addr.cur_addr_hi = 0; pb.audio_addr.cur_addr_lo = 0;
		pb.audio_addr.end_addr_hi = 0; pb.audio_addr.end_addr_lo = 0;
		pb.audio_addr.loop_addr_hi = 0; pb.audio_addr.loop_addr_lo = 0;

		pb.src.cur_addr_frac = 0; pb.src.ratio_hi = 0; pb.src.ratio_lo = 0;
		pb.adpcm.pred_scale = 0; pb.adpcm.yn1 = 0; pb.adpcm.yn2 = 0;

		pb.audio_addr.looping = 0;
		pb.adpcm_loop_info.pred_scale = 0;
		pb.adpcm_loop_info.yn1 = 0; pb.adpcm_loop_info.yn2 = 0;
	}

	/*
	// the fact that no settings are reset (except running) after a SSBM type music stream or another
	looping block (for example in Battle Stadium DON) has ended could cause loud garbled sound to be
	played from one or more blocks. Perhaps it was in conjunction with the old sequenced music fix below,
	I'm not sure. This was an attempt to prevent that anyway by resetting all. But I'm not sure if this
	is needed anymore. Please try to play SSBM without it and see if it works anyway.
	*/
	if (
		// detect blocks that have recently been running that we should reset
		pb.running == 0 && pb.audio_addr.looping == 1
		//pb.running == 0 && pb.adpcm_loop_info.pred_scale

		// this prevents us from ruining sequenced music blocks, may not be needed
		/*
		&& !(pb.updates.num_updates[0] || pb.updates.num_updates[1] || pb.updates.num_updates[2]
		|| pb.updates.num_updates[3] || pb.updates.num_updates[4])
		*/	
		//&& !(updpar || upddata)

		&& pb.mixer_control == 0	// only use this in SSBM
		)
	{
		// reset the detection values
		pb.audio_addr.looping = 0;
		pb.adpcm_loop_info.pred_scale = 0;
		pb.adpcm_loop_info.yn1 = 0; pb.adpcm_loop_info.yn2 = 0;

		//pb.audio_addr.cur_addr_hi = 0; pb.audio_addr.cur_addr_lo = 0;
		//pb.audio_addr.end_addr_hi = 0; pb.audio_addr.end_addr_lo = 0;
		//pb.audio_addr.loop_addr_hi = 0; pb.audio_addr.loop_addr_lo = 0;

		//pb.src.cur_addr_frac = 0; PBs[i].src.ratio_hi = 0; PBs[i].src.ratio_lo = 0;
		//pb.adpcm.pred_scale = 0; pb.adpcm.yn1 = 0; pb.adpcm.yn2 = 0;
	}
}

static void ApplyUpdatesForMs(AXPB& pb, int curr_ms)
{
	u32 start_idx = 0;
	for (int i = 0; i < curr_ms; ++i)
		start_idx += pb.updates.num_updates[i];

	u32 update_addr = HILO_TO_32(pb.updates.data);
	for (u32 i = start_idx; i < start_idx + pb.updates.num_updates[curr_ms]; ++i)
	{
		u16 update_off = HLEMemory_Read_U16(update_addr + 4 * i);
		u16 update_val = HLEMemory_Read_U16(update_addr + 4 * i + 2);

		((u16*)&pb)[update_off] = update_val;
	}
}

void CUCode_NewAX::SetupProcessing(u32 init_addr)
{
	u16 init_data[0x20];

	for (u32 i = 0; i < 0x20; ++i)
		init_data[i] = HLEMemory_Read_U16(init_addr + 2 * i);

	// List of all buffers we have to initialize
	int* buffers[] = {
		m_samples_left,
		m_samples_right,
		m_samples_surround,
		m_samples_auxA_left,
		m_samples_auxA_right,
		m_samples_auxA_surround,
		m_samples_auxB_left,
		m_samples_auxB_right,
		m_samples_auxB_surround
	};

	u32 init_idx = 0;
	for (u32 i = 0; i < sizeof (buffers) / sizeof (buffers[0]); ++i)
	{
		s32 init_val = (s32)((init_data[init_idx] << 16) | init_data[init_idx + 1]);
		s16 delta = (s16)init_data[init_idx + 2];

		init_idx += 3;

		if (!init_val)
			memset(buffers[i], 0, 5 * 32 * sizeof (int));
		else
		{
			for (u32 j = 0; j < 32 * 5; ++j)
			{
				buffers[i][j] = init_val;
				init_val -= delta;
			}
		}
	}
}

void CUCode_NewAX::ProcessPBList(u32 pb_addr)
{
	// Samples per millisecond. In theory DSP sampling rate can be changed from
	// 32KHz to 48KHz, but AX always process at 32KHz.
	const u32 spms = 32;

	AXPB pb;

	while (pb_addr)
	{
		AXBuffers buffers = {{
			m_samples_left,
			m_samples_right,
			m_samples_auxA_left,
			m_samples_auxA_right,
			m_samples_auxB_left,
			m_samples_auxB_right
		}};

		if (!ReadPB(pb_addr, pb))
			break;

		for (int curr_ms = 0; curr_ms < 5; ++curr_ms)
		{
			ApplyUpdatesForMs(pb, curr_ms);

			// TODO: is that still needed?
			if (m_CRC != 0x3389a79e)
				VoiceHacks(pb);

			MixAddVoice(pb, buffers, spms, false);

			// Forward the buffers
			for (u32 i = 0; i < sizeof (buffers.ptrs) / sizeof (buffers.ptrs[0]); ++i)
				buffers.ptrs[i] += spms;
		}

		WritePB(pb_addr, pb);
		pb_addr = HILO_TO_32(pb.next_pb);
	}
}

void CUCode_NewAX::MixAUXSamples(bool AUXA, u32 write_addr, u32 read_addr)
{
	int buffers[3][5 * 32];

	// First, we need to send the contents of our AUX buffers to the CPU.
	if (write_addr)
	{
		for (u32 i = 0; i < 5 * 32; ++i)
		{
			if (AUXA)
			{
				buffers[0][i] = Common::swap32(m_samples_auxA_left[i]);
				buffers[1][i] = Common::swap32(m_samples_auxA_right[i]);
				buffers[2][i] = Common::swap32(m_samples_auxA_surround[i]);
			}
			else
			{
				buffers[0][i] = Common::swap32(m_samples_auxB_left[i]);
				buffers[1][i] = Common::swap32(m_samples_auxB_right[i]);
				buffers[2][i] = Common::swap32(m_samples_auxB_surround[i]);
			}
		}
		memcpy(HLEMemory_Get_Pointer(write_addr), buffers, sizeof (buffers));
	}

	// Then, we read the new buffers from the CPU and add to our current
	// buffers.
	memcpy(buffers, HLEMemory_Get_Pointer(read_addr), sizeof (buffers));
	for (u32 i = 0; i < 5 * 32; ++i)
	{
		m_samples_left[i] += Common::swap32(buffers[0][i]);
		m_samples_right[i] += Common::swap32(buffers[1][i]);
		m_samples_surround[i] += Common::swap32(buffers[2][i]);
	}
}

void CUCode_NewAX::OutputSamples(u32 out_addr)
{
	// 32 samples per ms, 5 ms, 2 channels
	short buffer[5 * 32 * 2];

	// Clamp internal buffers to 16 bits.
	for (u32 i = 0; i < 5 * 32; ++i)
	{
		int left  = m_samples_left[i];
		int right = m_samples_right[i];

		if (left < -32767)  left = -32767;
		if (left > 32767)   left = 32767;
		if (right < -32767) right = -32767;
		if (right >  32767) right = 32767;

		m_samples_left[i] = left;
		m_samples_right[i] = right;
	}

	for (u32 i = 0; i < 5 * 32; ++i)
	{
		buffer[2 * i] = Common::swap16(m_samples_left[i]);
		buffer[2 * i + 1] = Common::swap16(m_samples_right[i]);
	}

	memcpy(HLEMemory_Get_Pointer(out_addr), buffer, sizeof (buffer));
}

void CUCode_NewAX::HandleMail(u32 mail)
{
	// Indicates if the next message is a command list address.
	static bool next_is_cmdlist = false;
	static u16 cmdlist_size = 0;

	bool set_next_is_cmdlist = false;

	// Wait for DSP processing to be done before answering any mail. This is
	// safe to do because it matches what the DSP does on real hardware: there
	// is no interrupt when a mail from CPU is received.
	m_processing.lock();

	if (next_is_cmdlist)
	{
		CopyCmdList(mail, cmdlist_size);
		NotifyAXThread();
	}
	else if (m_UploadSetupInProgress)
	{
		PrepareBootUCode(mail);
	}
	else if (mail == MAIL_RESUME)
	{
		// Acknowledge the resume request
		m_rMailHandler.PushMail(DSP_RESUME);
		DSP::GenerateDSPInterruptFromDSPEmu(DSP::INT_DSP);
	}
	else if (mail == MAIL_NEW_UCODE)
	{
		soundStream->GetMixer()->SetHLEReady(false);
		m_UploadSetupInProgress = true;
	}
	else if (mail == MAIL_RESET)
	{
		m_DSPHLE->SetUCode(UCODE_ROM);
	}
	else if (mail == MAIL_CONTINUE)
	{
		// We don't have to do anything here - the CPU does not wait for a ACK
		// and sends a cmdlist mail just after.
	}
	else if ((mail & MAIL_CMDLIST_MASK) == MAIL_CMDLIST)
	{
		// A command list address is going to be sent next.
		set_next_is_cmdlist = true;
		cmdlist_size = (u16)(mail & ~MAIL_CMDLIST_MASK);
	}
	else
	{
		ERROR_LOG(DSPHLE, "Unknown mail sent to AX::HandleMail: %08x", mail);
	}

	m_processing.unlock();
	next_is_cmdlist = set_next_is_cmdlist;
}

void CUCode_NewAX::CopyCmdList(u32 addr, u16 size)
{
	if (size >= (sizeof (m_cmdlist) / sizeof (u16)))
	{
		ERROR_LOG(DSPHLE, "Command list at %08x is too large: size=%d", addr, size);
		return;
	}

	for (u32 i = 0; i < size; ++i, addr += 2)
		m_cmdlist[i] = HLEMemory_Read_U16(addr);
	m_cmdlist_size = size;
}

void CUCode_NewAX::MixAdd(short* out_buffer, int nsamples)
{
	// Should never be called: we do not set HLE as ready.
	// We accurately send samples to RAM instead of directly to the mixer.
}

void CUCode_NewAX::Update(int cycles)
{
	// Used for UCode switching.
	if (NeedsResumeMail())
	{
		m_rMailHandler.PushMail(DSP_RESUME);
		DSP::GenerateDSPInterruptFromDSPEmu(DSP::INT_DSP);
	}
}

void CUCode_NewAX::DoState(PointerWrap& p)
{
	std::lock_guard<std::mutex> lk(m_processing);

	DoStateShared(p);
}
