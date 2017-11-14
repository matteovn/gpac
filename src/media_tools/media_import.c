/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre, Romain Bouqueau, Cyril Concolato
 *			Copyright (c) Telecom ParisTech 2000-2012
 *					All rights reserved
 *
 *  This file is part of GPAC / Media Tools sub-project
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <gpac/filters.h>


#include <gpac/internal/media_dev.h>
#ifndef GPAC_DISABLE_AVILIB
#include <gpac/internal/avilib.h>
#endif
#ifndef GPAC_DISABLE_OGG
#include <gpac/internal/ogg.h>
#endif
#ifndef GPAC_DISABLE_VOBSUB
#include <gpac/internal/vobsub.h>
#endif
#include <gpac/xml.h>
#include <gpac/mpegts.h>
#include <gpac/constants.h>
#include <gpac/base_coding.h>
#include <gpac/internal/isomedia_dev.h>


#ifndef GPAC_DISABLE_MEDIA_IMPORT


GF_Err gf_import_message(GF_MediaImporter *import, GF_Err e, char *format, ...)
{
#ifndef GPAC_DISABLE_LOG
	if (gf_log_tool_level_on(GF_LOG_AUTHOR, e ? GF_LOG_WARNING : GF_LOG_INFO)) {
		va_list args;
		char szMsg[1024];
		va_start(args, format);
		vsprintf(szMsg, format, args);
		va_end(args);
		GF_LOG((u32) (e ? GF_LOG_WARNING : GF_LOG_INFO), GF_LOG_AUTHOR, ("%s\n", szMsg) );
	}
#endif
	return e;
}


static GF_Err gf_media_update_par(GF_ISOFile *file, u32 track)
{
#ifndef GPAC_DISABLE_AV_PARSERS
	u32 tk_w, tk_h, stype;
	GF_Err e;

	e = gf_isom_get_visual_info(file, track, 1, &tk_w, &tk_h);
	if (e) return e;

	stype = gf_isom_get_media_subtype(file, track, 1);
	if ((stype==GF_ISOM_SUBTYPE_AVC_H264) || (stype==GF_ISOM_SUBTYPE_AVC2_H264)
	        || (stype==GF_ISOM_SUBTYPE_AVC3_H264) || (stype==GF_ISOM_SUBTYPE_AVC4_H264)
	   ) {
		s32 par_n, par_d;
		GF_AVCConfig *avcc = gf_isom_avc_config_get(file, track, 1);
		GF_AVCConfigSlot *slc = (GF_AVCConfigSlot *)gf_list_get(avcc->sequenceParameterSets, 0);
		par_n = par_d = 1;
		if (slc) gf_avc_get_sps_info(slc->data, slc->size, NULL, NULL, NULL, &par_n, &par_d);
		gf_odf_avc_cfg_del(avcc);

		if ((par_n>1) && (par_d>1))
			tk_w = tk_w * par_n / par_d;
	}
	else if ((stype==GF_ISOM_SUBTYPE_MPEG4) || (stype==GF_ISOM_SUBTYPE_MPEG4_CRYP) ) {
		GF_M4VDecSpecInfo dsi;
		GF_ESD *esd = gf_isom_get_esd(file, track, 1);
		if (!esd || !esd->decoderConfig || (esd->decoderConfig->streamType!=4) || (esd->decoderConfig->objectTypeIndication!=GPAC_OTI_VIDEO_MPEG4_PART2)) {
			if (esd) gf_odf_desc_del((GF_Descriptor *) esd);
			return GF_NOT_SUPPORTED;
		}
		gf_m4v_get_config(esd->decoderConfig->decoderSpecificInfo->data, esd->decoderConfig->decoderSpecificInfo->dataLength, &dsi);
		if (esd) gf_odf_desc_del((GF_Descriptor *) esd);

		if ((dsi.par_num>1) && (dsi.par_den>1))
			tk_w = dsi.width * dsi.par_num / dsi.par_den;
	} else {
		return GF_OK;
	}
	return gf_isom_set_track_layout_info(file, track, tk_w<<16, tk_h<<16, 0, 0, 0);
#else
	return GF_OK;
#endif /*GPAC_DISABLE_AV_PARSERS*/
}


void gf_media_update_bitrate(GF_ISOFile *file, u32 track)
{
#ifndef GPAC_DISABLE_ISOM_WRITE
	u32 i, count, timescale, db_size;
	u64 time_wnd, rate, max_rate, avg_rate, bitrate;
	Double br;
	GF_ESD *esd;
	GF_ISOSample sample;
	db_size = 0;

	esd = gf_isom_get_esd(file, track, 1);
	if (esd) {
		db_size = esd->decoderConfig->bufferSizeDB;
		esd->decoderConfig->avgBitrate = 0;
		esd->decoderConfig->maxBitrate = 0;
	}
	rate = max_rate = avg_rate = time_wnd = 0;

	memset(&sample, 0, sizeof(GF_ISOSample));
	timescale = gf_isom_get_media_timescale(file, track);
	count = gf_isom_get_sample_count(file, track);
	for (i=0; i<count; i++) {
		u32 di;
		GF_ISOSample *samp = gf_isom_get_sample_info_ex(file, track, i+1, &di, NULL, &sample);
		if (!samp) break;

		if (samp->dataLength > db_size) db_size = samp->dataLength;

		avg_rate += samp->dataLength;
		rate += samp->dataLength;
		if (samp->DTS > time_wnd + timescale) {
			if (rate > max_rate) max_rate = rate;
			time_wnd = samp->DTS;
			rate = 0;
		}
	}

	br = (Double) (s64) gf_isom_get_media_duration(file, track);
	br /= timescale;
	bitrate = (u32) ((Double) (s64)avg_rate / br);
	bitrate *= 8;
	max_rate *= 8;

	/*move to bps*/
	if (esd) {
		esd->decoderConfig->avgBitrate = (u32) bitrate;
		esd->decoderConfig->maxBitrate = (u32) max_rate;
		esd->decoderConfig->bufferSizeDB = db_size;
		gf_isom_change_mpeg4_description(file, track, 1, esd);
		gf_odf_desc_del((GF_Descriptor *)esd);
	} else {
		gf_isom_update_bitrate(file, track, 1, (u32) bitrate, (u32) max_rate, db_size);
	}

#endif
}

static void get_video_timing(Double fps, u32 *timescale, u32 *dts_inc)
{
	u32 fps_1000 = (u32) (fps*1000 + 0.5);
	/*handle all drop-frame formats*/
	if (fps_1000==29970) {
		*timescale = 30000;
		*dts_inc = 1001;
	}
	else if (fps_1000==23976) {
		*timescale = 24000;
		*dts_inc = 1001;
	}
	else if (fps_1000==59940) {
		*timescale = 60000;
		*dts_inc = 1001;
	} else {
		*timescale = fps_1000;
		*dts_inc = 1000;
	}
}

static GF_Err gf_import_afx_sc3dmc(GF_MediaImporter *import, Bool mult_desc_allowed)
{
	GF_Err e;
	Bool destroy_esd;
	u32 size, track, di, dsi_len;
	GF_ISOSample *samp;
	u8 OTI;
	char *dsi, *data;
	FILE *src;

	if (import->flags & GF_IMPORT_PROBE_ONLY) {
		import->tk_info[0].track_num = 1;
		import->tk_info[0].stream_type = GF_STREAM_SCENE;
		import->tk_info[0].media_oti = GPAC_OTI_SCENE_AFX;
		import->tk_info[0].flags = GF_IMPORT_USE_DATAREF | GF_IMPORT_NO_DURATION;
		import->nb_tracks = 1;
		return GF_OK;
	}

	src = gf_fopen(import->in_name, "rb");
	if (!src) return gf_import_message(import, GF_URL_ERROR, "Opening file %s failed", import->in_name);

	gf_fseek(src, 0, SEEK_END);
	size = (u32) gf_ftell(src);
	gf_fseek(src, 0, SEEK_SET);
	data = (char*)gf_malloc(sizeof(char)*size);
	size = (u32) fread(data, sizeof(char), size, src);
	gf_fclose(src);
	if ((s32) size < 0) return GF_IO_ERR;

	OTI = GPAC_OTI_SCENE_AFX;

	dsi = (char *)gf_malloc(1);
	dsi_len = 1;
	dsi[0] = GPAC_AFX_SCALABLE_COMPLEXITY;

	destroy_esd = GF_FALSE;
	if (!import->esd) {
		import->esd = gf_odf_desc_esd_new(0);
		destroy_esd = GF_TRUE;
	}
	/*update stream type/oti*/
	if (!import->esd->decoderConfig) import->esd->decoderConfig = (GF_DecoderConfig *) gf_odf_desc_new(GF_ODF_DCD_TAG);
	if (!import->esd->slConfig) import->esd->slConfig = (GF_SLConfig *) gf_odf_desc_new(GF_ODF_SLC_TAG);
	import->esd->decoderConfig->streamType = GF_STREAM_SCENE;
	import->esd->decoderConfig->objectTypeIndication = OTI;
	import->esd->decoderConfig->bufferSizeDB = size;
	import->esd->decoderConfig->avgBitrate = 8*size;
	import->esd->decoderConfig->maxBitrate = 8*size;
	import->esd->slConfig->timestampResolution = 1000;

	if (dsi) {
		if (!import->esd->decoderConfig->decoderSpecificInfo) import->esd->decoderConfig->decoderSpecificInfo = (GF_DefaultDescriptor *) gf_odf_desc_new(GF_ODF_DSI_TAG);
		if (import->esd->decoderConfig->decoderSpecificInfo->data) gf_free(import->esd->decoderConfig->decoderSpecificInfo->data);
		import->esd->decoderConfig->decoderSpecificInfo->data = dsi;
		import->esd->decoderConfig->decoderSpecificInfo->dataLength = dsi_len;
	}


	track = 0;
	if (mult_desc_allowed)
		track = gf_isom_get_track_by_id(import->dest, import->esd->ESID);
	if (!track)
		track = gf_isom_new_track(import->dest, import->esd->ESID, GF_ISOM_MEDIA_SCENE, 1000);
	if (!track) {
		e = gf_isom_last_error(import->dest);
		goto exit;
	}
	gf_isom_set_track_enabled(import->dest, track, 1);
	if (!import->esd->ESID) import->esd->ESID = gf_isom_get_track_id(import->dest, track);
	import->final_trackID = import->esd->ESID;

	e = gf_isom_new_mpeg4_description(import->dest, track, import->esd, (import->flags & GF_IMPORT_USE_DATAREF) ? import->in_name : NULL, NULL, &di);
	if (e) goto exit;
	//gf_isom_set_visual_info(import->dest, track, di, w, h);
	samp = gf_isom_sample_new();
	samp->IsRAP = RAP;
	samp->dataLength = size;
	if (import->initial_time_offset) samp->DTS = (u64) (import->initial_time_offset*1000);

	gf_import_message(import, GF_OK, "%s import %s", "SC3DMC", import->in_name);

	/*we must start a track from DTS = 0*/
	if (!gf_isom_get_sample_count(import->dest, track) && samp->DTS) {
		/*todo - we could add an edit list*/
		samp->DTS=0;
	}

	gf_set_progress("Importing SC3DMC", 0, 1);
	if (import->flags & GF_IMPORT_USE_DATAREF) {
		e = gf_isom_add_sample_reference(import->dest, track, di, samp, (u64) 0);
	} else {
		samp->data = data;
		e = gf_isom_add_sample(import->dest, track, di, samp);
		samp->data = NULL;
	}
	gf_set_progress("Importing SC3DMC", 1, 1);

	gf_isom_sample_del(&samp);

exit:
	gf_free(data);
	if (import->esd && destroy_esd) {
		gf_odf_desc_del((GF_Descriptor *) import->esd);
		import->esd = NULL;
	}
	return e;
}


#ifdef FILTER_FIXME

#ifndef GPAC_DISABLE_AV_PARSERS

static Bool LOAS_LoadFrame(GF_BitStream *bs, GF_M4ADecSpecInfo *acfg, u32 *nb_bytes, u8 *buffer)
{
	u32 val, size;
	u64 pos, mux_size;
	if (!acfg) return 0;
	memset(acfg, 0, sizeof(GF_M4ADecSpecInfo));
	while (gf_bs_available(bs)) {
		val = gf_bs_read_u8(bs);
		if (val!=0x56) continue;
		val = gf_bs_read_int(bs, 3);
		if (val != 0x07) {
			gf_bs_read_int(bs, 5);
			continue;
		}
		mux_size = gf_bs_read_int(bs, 13);
		pos = gf_bs_get_position(bs);

		/*use same stream mux*/
		if (!gf_bs_read_int(bs, 1)) {
			Bool amux_version, amux_versionA;

			amux_version = (Bool)gf_bs_read_int(bs, 1);
			amux_versionA = GF_FALSE;
			if (amux_version) amux_versionA = (Bool)gf_bs_read_int(bs, 1);
			if (!amux_versionA) {
				u32 i, allStreamsSameTimeFraming, numProgram;
				if (amux_version) gf_latm_get_value(bs);

				allStreamsSameTimeFraming = gf_bs_read_int(bs, 1);
				/*numSubFrames = */gf_bs_read_int(bs, 6);
				numProgram = gf_bs_read_int(bs, 4);
				for (i=0; i<=numProgram; i++) {
					u32 j, num_lay;
					num_lay = gf_bs_read_int(bs, 3);
					for (j=0; j<=num_lay; j++) {
						u32 frameLengthType;
						Bool same_cfg = GF_FALSE;
						if (i || j) same_cfg = (Bool)gf_bs_read_int(bs, 1);

						if (!same_cfg) {
							if (amux_version==1) gf_latm_get_value(bs);
							gf_m4a_parse_config(bs, acfg, GF_FALSE);
						}
						frameLengthType = gf_bs_read_int(bs, 3);
						if (!frameLengthType) {
							/*latmBufferFullness = */gf_bs_read_int(bs, 8);
							if (!allStreamsSameTimeFraming) {
							}
						} else {
							/*not supported*/
						}
					}

				}
				/*other data present*/
				if (gf_bs_read_int(bs, 1)) {
//					u32 k = 0;
				}
				/*CRCcheck present*/
				if (gf_bs_read_int(bs, 1)) {
				}
			}
		}

		size = 0;
		while (1) {
			u32 tmp = gf_bs_read_int(bs, 8);
			size += tmp;
			if (tmp!=255) break;
		}
		if (nb_bytes && buffer) {
			*nb_bytes = (u32) size;
			gf_bs_read_data(bs, (char *) buffer, size);
		} else {
			gf_bs_skip_bytes(bs, size);
		}

		/*parse amux*/
		gf_bs_seek(bs, pos + mux_size);

		if (gf_bs_peek_bits(bs, 11, 0) != 0x2B7) {
			gf_bs_seek(bs, pos + 1);
			continue;
		}

		return GF_TRUE;
	}
	return GF_FALSE;
}

static GF_Err gf_import_aac_loas(GF_MediaImporter *import)
{
	u8 oti;
	Bool destroy_esd;
	GF_Err e;
	Bool sync_frame;
	u16 sr, dts_inc;
	u32 timescale;
	GF_BitStream *bs, *dsi;
	GF_M4ADecSpecInfo acfg;
	FILE *in;
	u32 nbbytes=0;
	u8 aac_buf[4096];
	u64 tot_size, done, duration;
	u32 track, di;
	GF_ISOSample *samp;

	in = gf_fopen(import->in_name, "rb");
	if (!in) return gf_import_message(import, GF_URL_ERROR, "Opening file %s failed", import->in_name);

	bs = gf_bs_from_file(in, GF_BITSTREAM_READ);

	/*sync_frame = */LOAS_LoadFrame(bs, &acfg, &nbbytes, (u8 *)aac_buf);

	/*keep MPEG-2 AAC OTI even for HE-SBR (that's correct according to latest MPEG-4 audio spec)*/
	oti = GPAC_OTI_AUDIO_AAC_MPEG4;
	timescale = sr = acfg.base_sr;

	if (import->flags & GF_IMPORT_PROBE_ONLY) {
		import->tk_info[0].track_num = 1;
		import->tk_info[0].stream_type = GF_STREAM_AUDIO;
		import->tk_info[0].flags = GF_IMPORT_SBR_IMPLICIT | GF_IMPORT_SBR_EXPLICIT | GF_IMPORT_PS_IMPLICIT | GF_IMPORT_PS_EXPLICIT | GF_IMPORT_FORCE_MPEG4;
		import->nb_tracks = 1;
		import->tk_info[0].audio_info.sample_rate = sr;
		import->tk_info[0].audio_info.nb_channels = acfg.nb_chan;
		gf_bs_del(bs);
		gf_fclose(in);
		return GF_OK;
	}

	dsi = gf_bs_new(NULL, 0, GF_BITSTREAM_WRITE);
	gf_m4a_write_config_bs(dsi, &acfg);

	if (import->flags & GF_IMPORT_PS_EXPLICIT) {
		import->flags &= ~GF_IMPORT_PS_IMPLICIT;
		import->flags |= GF_IMPORT_SBR_EXPLICIT;
		import->flags &= ~GF_IMPORT_SBR_IMPLICIT;
	}

	dts_inc = 1024;

	destroy_esd = GF_FALSE;
	if (!import->esd) {
		import->esd = gf_odf_desc_esd_new(2);
		destroy_esd = GF_TRUE;
	}
	if (!import->esd->decoderConfig) import->esd->decoderConfig = (GF_DecoderConfig *) gf_odf_desc_new(GF_ODF_DCD_TAG);
	if (!import->esd->slConfig) import->esd->slConfig = (GF_SLConfig *) gf_odf_desc_new(GF_ODF_SLC_TAG);
	import->esd->decoderConfig->streamType = GF_STREAM_AUDIO;
	import->esd->decoderConfig->objectTypeIndication = oti;
	import->esd->decoderConfig->bufferSizeDB = 20;
	import->esd->slConfig->timestampResolution = timescale;
	if (!import->esd->decoderConfig->decoderSpecificInfo) import->esd->decoderConfig->decoderSpecificInfo = (GF_DefaultDescriptor *) gf_odf_desc_new(GF_ODF_DSI_TAG);
	if (import->esd->decoderConfig->decoderSpecificInfo->data) gf_free(import->esd->decoderConfig->decoderSpecificInfo->data);
	gf_bs_get_content(dsi, &import->esd->decoderConfig->decoderSpecificInfo->data, &import->esd->decoderConfig->decoderSpecificInfo->dataLength);
	gf_bs_del(dsi);

	samp = NULL;
	gf_import_message(import, GF_OK, "MPEG-4 AAC in LOAS import - sample rate %d - %d channel%s", sr, acfg.nb_chan, (acfg.nb_chan > 1) ? "s" : "");

	track = gf_isom_new_track(import->dest, import->esd->ESID, GF_ISOM_MEDIA_AUDIO, timescale);
	if (!track) {
		e = gf_isom_last_error(import->dest);
		goto exit;
	}
	gf_isom_set_track_enabled(import->dest, track, 1);
	if (!import->esd->ESID) import->esd->ESID = gf_isom_get_track_id(import->dest, track);
	import->final_trackID = import->esd->ESID;
	gf_isom_new_mpeg4_description(import->dest, track, import->esd, (import->flags & GF_IMPORT_USE_DATAREF) ? import->in_name : NULL, NULL, &di);
	gf_isom_set_audio_info(import->dest, track, di, timescale, (acfg.nb_chan>2) ? 2 : acfg.nb_chan, 16);

	/*add first sample*/
	samp = gf_isom_sample_new();
	samp->IsRAP = RAP;
	samp->dataLength = nbbytes;
	samp->data = (char *) aac_buf;

	e = gf_isom_add_sample(import->dest, track, di, samp);
	if (e) goto exit;
	samp->DTS+=dts_inc;

	duration = import->duration;
	duration *= sr;
	duration /= 1000;

	tot_size = gf_bs_get_size(bs);
	done = 0;
	while (gf_bs_available(bs) ) {
		sync_frame = LOAS_LoadFrame(bs, &acfg, &nbbytes, (u8 *)aac_buf);
		if (!sync_frame) break;

		samp->data = (char*)aac_buf;
		samp->dataLength = nbbytes;

		e = gf_isom_add_sample(import->dest, track, di, samp);
		if (e) break;

		gf_set_progress("Importing AAC", done, tot_size);
		samp->DTS += dts_inc;
		done += samp->dataLength;
		if (duration && (samp->DTS > duration)) break;
		if (import->flags & GF_IMPORT_DO_ABORT) break;
	}
	gf_media_update_bitrate(import->dest, track);
	gf_isom_set_pl_indication(import->dest, GF_ISOM_PL_AUDIO, acfg.audioPL);
	gf_set_progress("Importing AAC", tot_size, tot_size);

exit:
	if (import->esd && destroy_esd) {
		gf_odf_desc_del((GF_Descriptor *) import->esd);
		import->esd = NULL;
	}
	if (samp) {
		samp->data = NULL;
		gf_isom_sample_del(&samp);
	}
	gf_bs_del(bs);
	gf_fclose(in);
	return e;
}


#endif /*GPAC_DISABLE_AV_PARSERS*/

#endif // FILTER_FIXME


static void update_edit_list_for_bframes(GF_ISOFile *file, u32 track)
{
	u32 i, count, di;
	u64 max_cts, min_cts, doff;

	count = gf_isom_get_sample_count(file, track);
	max_cts = 0;
	min_cts = (u64) -1;
	for (i=0; i<count; i++) {
		GF_ISOSample *s = gf_isom_get_sample_info(file, track, i+1, &di, &doff);
		if (s->DTS + s->CTS_Offset > max_cts)
			max_cts = s->DTS + s->CTS_Offset;

		if (min_cts > s->DTS + s->CTS_Offset)
			min_cts = s->DTS + s->CTS_Offset;

		gf_isom_sample_del(&s);
	}

	if (min_cts) {
		max_cts -= min_cts;
		max_cts += gf_isom_get_sample_duration(file, track, count);

		max_cts *= gf_isom_get_timescale(file);
		max_cts /= gf_isom_get_media_timescale(file, track);
		gf_isom_set_edit_segment(file, track, 0, max_cts, min_cts, GF_ISOM_EDIT_NORMAL);
	}
}

#ifndef GPAC_DISABLE_AVILIB

static GF_Err gf_import_avi_video(GF_MediaImporter *import)
{
	GF_Err e;
	Double FPS;
	FILE *test;
	GF_ISOSample *samp;
	u32 i, num_samples, timescale, track, di, PL, max_b, nb_f, ref_frame, b_frames;
	u64 samp_offset, size, max_size;
	u32 nbI, nbP, nbB, nbDummy, nbNotCoded, dts_inc, cur_samp;
	Bool is_vfr, erase_pl;
	GF_M4VDecSpecInfo dsi;
	GF_M4VParser *vparse;
	s32 key;
	u64 duration;
	Bool destroy_esd, is_packed, is_init, has_cts_offset;
	char *comp, *frame;
	avi_t *in;

	test = gf_fopen(import->in_name, "rb");
	if (!test) return gf_import_message(import, GF_URL_ERROR, "Opening %s failed", import->in_name);
	gf_fclose(test);
	in = AVI_open_input_file(import->in_name, 1);
	if (!in) return gf_import_message(import, GF_NON_COMPLIANT_BITSTREAM, "Unsupported avi file");

	if (import->flags & GF_IMPORT_PROBE_ONLY) {
		char *comp;
		import->tk_info[0].track_num = 1;
		import->tk_info[0].stream_type = GF_STREAM_VISUAL;
		import->tk_info[0].flags = GF_IMPORT_USE_DATAREF | GF_IMPORT_NO_FRAME_DROP | GF_IMPORT_OVERRIDE_FPS;
		import->tk_info[0].video_info.FPS = AVI_frame_rate(in);
		import->tk_info[0].video_info.width = AVI_video_width(in);
		import->tk_info[0].video_info.height = AVI_video_height(in);
		comp = AVI_video_compressor(in);
		import->tk_info[0].media_4cc = GF_4CC((u8)comp[0], (u8)comp[1], (u8)comp[2], (u8)comp[3]);

		import->nb_tracks = 1;
		for (i=0; i<(u32) AVI_audio_tracks(in); i++) {
			import->tk_info[i+1].track_num = i+2;
			import->tk_info[i+1].stream_type = GF_STREAM_AUDIO;
			import->tk_info[i+1].flags = GF_IMPORT_USE_DATAREF;
			import->tk_info[i+1].audio_info.sample_rate = (u32) AVI_audio_rate(in);
			import->tk_info[i+1].audio_info.nb_channels = (u32) AVI_audio_channels(in);
			import->nb_tracks ++;
		}
		AVI_close(in);
		return GF_OK;
	}
	if (import->trackID>1) {
		AVI_close(in);
		return GF_OK;
	}
	destroy_esd = GF_FALSE;
	frame = NULL;
	AVI_seek_start(in);

	erase_pl = GF_FALSE;
	comp = AVI_video_compressor(in);
	if (!comp) {
		e = GF_NOT_SUPPORTED;
		goto exit;
	}

	/*these are/should be OK*/
	if (!stricmp(comp, "DIVX") || !stricmp(comp, "DX50")	/*DivX*/
	        || !stricmp(comp, "XVID") /*XviD*/
	        || !stricmp(comp, "3iv2") /*3ivX*/
	        || !stricmp(comp, "fvfw") /*ffmpeg*/
	        || !stricmp(comp, "NDIG") /*nero*/
	        || !stricmp(comp, "MP4V") /*!! not tested*/
	        || !stricmp(comp, "M4CC") /*Divio - not tested*/
	        || !stricmp(comp, "PVMM") /*PacketVideo - not tested*/
	        || !stricmp(comp, "SEDG") /*Samsung - not tested*/
	        || !stricmp(comp, "RMP4") /*Sigma - not tested*/
	        || !stricmp(comp, "MP43") /*not tested*/
	        || !stricmp(comp, "FMP4") /*not tested*/
	   ) {

	}
	else if (!stricmp(comp, "DIV3") || !stricmp(comp, "DIV4")) {
		gf_import_message(import, GF_NOT_SUPPORTED, "Video format %s not compliant with MPEG-4 Visual - please recompress the file first", comp);
		e = GF_NOT_SUPPORTED;
		goto exit;
	} else if (!stricmp(comp, "H264") || !stricmp(comp, "X264")) {
		gf_import_message(import, GF_NOT_SUPPORTED, "H264/AVC Video format not supported in AVI - please extract to raw format first", comp);
		e = GF_NOT_SUPPORTED;
		goto exit;
	} else {
		gf_import_message(import, GF_NOT_SUPPORTED, "Video format %s not supported - recompress the file first", comp);
		e = GF_NOT_SUPPORTED;
		goto exit;
	}
	/*no auto frame-rate detection*/
	if (import->video_fps == GF_IMPORT_AUTO_FPS)
		import->video_fps = GF_IMPORT_DEFAULT_FPS;

	FPS = AVI_frame_rate(in);
	if (import->video_fps) FPS = (Double) import->video_fps;
	get_video_timing(FPS, &timescale, &dts_inc);
	duration = (u64) (import->duration*FPS);

	e = GF_OK;
	max_size = 0;
	samp_offset = 0;
	frame = NULL;
	num_samples = (u32) AVI_video_frames(in);
	samp = gf_isom_sample_new();
	PL = 0;
	track = 0;
	is_vfr = GF_FALSE;

	is_packed = GF_FALSE;
	nbDummy = nbNotCoded = nbI = nbP = nbB = max_b = 0;
	has_cts_offset = GF_FALSE;
	cur_samp = b_frames = ref_frame = 0;

	is_init = GF_FALSE;

	for (i=0; i<num_samples; i++) {
		size = AVI_frame_size(in, i);
		if (!size) {
			AVI_read_frame(in, NULL, &key);
			continue;
		}

		if (size > max_size) {
			frame = (char*)gf_realloc(frame, sizeof(char) * (size_t)size);
			max_size = size;
		}
		AVI_read_frame(in, frame, &key);

		/*get DSI*/
		if (!is_init) {
			is_init = GF_TRUE;
			vparse = gf_m4v_parser_new(frame, size, GF_FALSE);
			e = gf_m4v_parse_config(vparse, &dsi);
			PL = dsi.VideoPL;
			if (!PL) {
				PL = 0x01;
				erase_pl = GF_TRUE;
			}
			samp_offset = gf_m4v_get_object_start(vparse);
			assert(samp_offset < 1<<31);
			gf_m4v_parser_del(vparse);
			if (e) {
				gf_import_message(import, e, "Cannot import decoder config in first frame");
				goto exit;
			}

			if (!import->esd) {
				import->esd = gf_odf_desc_esd_new(0);
				destroy_esd = GF_TRUE;
			}
			track = gf_isom_new_track(import->dest, import->esd->ESID, GF_ISOM_MEDIA_VISUAL, timescale);
			if (!track) {
				e = gf_isom_last_error(import->dest);
				goto exit;
			}
			gf_isom_set_track_enabled(import->dest, track, 1);
			if (!import->esd->ESID) import->esd->ESID = gf_isom_get_track_id(import->dest, track);
			import->final_trackID = gf_isom_get_track_id(import->dest, track);

			if (!import->esd->slConfig) import->esd->slConfig = (GF_SLConfig *) gf_odf_desc_new(GF_ODF_SLC_TAG);
			import->esd->slConfig->timestampResolution = timescale;

			if (!import->esd->decoderConfig) import->esd->decoderConfig = (GF_DecoderConfig *) gf_odf_desc_new(GF_ODF_DCD_TAG);
			if (import->esd->decoderConfig->decoderSpecificInfo) gf_odf_desc_del((GF_Descriptor *) import->esd->decoderConfig->decoderSpecificInfo);
			import->esd->decoderConfig->decoderSpecificInfo = (GF_DefaultDescriptor *) gf_odf_desc_new(GF_ODF_DSI_TAG);
			import->esd->decoderConfig->streamType = GF_STREAM_VISUAL;
			import->esd->decoderConfig->objectTypeIndication = GPAC_OTI_VIDEO_MPEG4_PART2;
			import->esd->decoderConfig->decoderSpecificInfo->data = (char *) gf_malloc(sizeof(char) * (size_t)samp_offset);
			memcpy(import->esd->decoderConfig->decoderSpecificInfo->data, frame, sizeof(char) * (size_t)samp_offset);
			import->esd->decoderConfig->decoderSpecificInfo->dataLength = (u32) samp_offset;

			gf_isom_set_cts_packing(import->dest, track, GF_TRUE);

			/*remove packed flag if any (VOSH user data)*/
			while (1) {
				char *divx_mark;
				while ((i+3<samp_offset)  && ((frame[i]!=0) || (frame[i+1]!=0) || (frame[i+2]!=1))) i++;
				if (i+4>=samp_offset) break;

				if (strncmp(frame+i+4, "DivX", 4)) {
					i += 4;
					continue;
				}
				divx_mark = import->esd->decoderConfig->decoderSpecificInfo->data + i + 4;
				divx_mark = strchr(divx_mark, 'p');
				if (divx_mark) divx_mark[0] = 'n';
				break;
			}
			i = 0;

			e = gf_isom_new_mpeg4_description(import->dest, track, import->esd, (import->flags & GF_IMPORT_USE_DATAREF) ? import->in_name: NULL, NULL, &di);
			if (e) goto exit;
			gf_isom_set_visual_info(import->dest, track, di, dsi.width, dsi.height);
			gf_import_message(import, GF_OK, "AVI %s video import - %d x %d @ %02.4f FPS - %d Frames\nIndicated Profile: %s", comp, dsi.width, dsi.height, FPS, num_samples, gf_m4v_get_profile_name((u8) PL));

			gf_media_update_par(import->dest, track);
		}


		if (size > samp_offset) {
			u8 ftype;
			u32 tinc;
			u64 framesize, frame_start;
			u64 file_offset;
			Bool is_coded;

			size -= samp_offset;
			file_offset = (u64) AVI_get_video_position(in, i);

			vparse = gf_m4v_parser_new(frame + samp_offset, size, GF_FALSE);

			samp->dataLength = 0;
			/*removing padding frames*/
			if (size<4) {
				nbDummy ++;
				size = 0;
			}

			nb_f=0;
			while (size) {
				GF_Err e = gf_m4v_parse_frame(vparse, dsi, &ftype, &tinc, &framesize, &frame_start, &is_coded);
				if (e<0) goto exit;

				if (!is_coded) {
					if (!gf_m4v_is_valid_object_type(vparse)) gf_import_message(import, GF_OK, "WARNING: AVI frame %d doesn't look like MPEG-4 Visual", i+1);
					nbNotCoded ++;
					if (!is_packed) {
						is_vfr = GF_TRUE;
						/*policy is to import at constant frame rate from AVI*/
						if (import->flags & GF_IMPORT_NO_FRAME_DROP) goto proceed;
						/*policy is to import at variable frame rate from AVI*/
						samp->DTS += dts_inc;
					}
					/*assume this is packed bitstream n-vop and discard it.*/
				} else {
proceed:
					if (e==GF_EOS) size = 0;
					else is_packed = GF_TRUE;
					nb_f++;

					samp->IsRAP = RAP_NO;

					if (ftype==2) {
						b_frames ++;
						nbB++;
						/*adjust CTS*/
						if (!has_cts_offset) {
							u32 i;
							for (i=0; i<gf_isom_get_sample_count(import->dest, track); i++) {
								gf_isom_modify_cts_offset(import->dest, track, i+1, dts_inc);
							}
							has_cts_offset = GF_TRUE;
						}
					} else {
						if (!ftype) {
							samp->IsRAP = RAP;
							nbI++;
						} else {
							nbP++;
						}
						/*even if no out-of-order frames we MUST adjust CTS if cts_offset is present is */
						if (ref_frame && has_cts_offset)
							gf_isom_modify_cts_offset(import->dest, track, ref_frame, (1+b_frames)*dts_inc);

						ref_frame = cur_samp+1;
						if (max_b<b_frames) max_b = b_frames;
						b_frames = 0;
					}
					/*frame_start indicates start of VOP (eg we always remove VOL from each I)*/
					samp->data = frame + samp_offset + frame_start;
					assert(framesize < 1<<31);
					samp->dataLength = (u32) framesize;

					if (import->flags & GF_IMPORT_USE_DATAREF) {
						samp->data = NULL;
						e = gf_isom_add_sample_reference(import->dest, track, di, samp, file_offset + samp_offset + frame_start);
					} else {
						e = gf_isom_add_sample(import->dest, track, di, samp);
					}
					cur_samp++;
					samp->DTS += dts_inc;
					if (e) {
						gf_import_message(import, GF_OK, "Error importing AVI frame %d", i+1);
						goto exit;
					}
				}
				if (!size || (size == framesize + frame_start)) break;
			}
			gf_m4v_parser_del(vparse);
			if (nb_f>2) gf_import_message(import, GF_OK, "Warning: more than 2 frames packed together");
		}
		samp_offset = 0;
		gf_set_progress("Importing AVI Video", i, num_samples);
		if (duration && (samp->DTS > duration)) break;
		if (import->flags & GF_IMPORT_DO_ABORT)
			break;
	}

	/*final flush*/
	if (ref_frame && has_cts_offset)
		gf_isom_modify_cts_offset(import->dest, track, ref_frame, (1+b_frames)*dts_inc);

	gf_set_progress("Importing AVI Video", num_samples, num_samples);


	num_samples = gf_isom_get_sample_count(import->dest, track);
	if (has_cts_offset) {
		gf_import_message(import, GF_OK, "Has B-Frames (%d max consecutive B-VOPs%s)", max_b, is_packed ? " - packed bitstream" : "");
		/*repack CTS tables and adjust offsets for B-frames*/
		gf_isom_set_cts_packing(import->dest, track, GF_FALSE);

		if (!(import->flags & GF_IMPORT_NO_EDIT_LIST))
			update_edit_list_for_bframes(import->dest, track);

		/*this is plain ugly but since some encoders (divx) don't use the video PL correctly
		we force the system video_pl to ASP@L5 since we have I, P, B in base layer*/
		if (PL<=3) {
			PL = 0xF5;
			erase_pl = GF_TRUE;
			gf_import_message(import, GF_OK, "WARNING: indicated profile doesn't include B-VOPs - forcing %s", gf_m4v_get_profile_name((u8) PL));
		}
		gf_import_message(import, GF_OK, "Import results: %d VOPs (%d Is - %d Ps - %d Bs)", num_samples, nbI, nbP, nbB);
	} else {
		/*no B-frames, remove CTS offsets*/
		gf_isom_remove_cts_info(import->dest, track);
		gf_import_message(import, GF_OK, "Import results: %d VOPs (%d Is - %d Ps)", num_samples, nbI, nbP);
	}

	samp->data = NULL;
	gf_isom_sample_del(&samp);

	if (erase_pl) {
		gf_m4v_rewrite_pl(&import->esd->decoderConfig->decoderSpecificInfo->data, &import->esd->decoderConfig->decoderSpecificInfo->dataLength, (u8) PL);
		gf_isom_change_mpeg4_description(import->dest, track, 1, import->esd);
	}
	gf_media_update_bitrate(import->dest, track);

	if (is_vfr) {
		if (nbB) {
			if (is_packed) gf_import_message(import, GF_OK, "Warning: Mix of non-coded frames: packed bitstream and encoder skiped - unpredictable timing");
		} else {
			if (import->flags & GF_IMPORT_NO_FRAME_DROP) {
				if (nbNotCoded) gf_import_message(import, GF_OK, "Stream has %d N-VOPs", nbNotCoded);
			} else {
				gf_import_message(import, GF_OK, "import using Variable Frame Rate - Removed %d N-VOPs", nbNotCoded);
			}
			nbNotCoded = 0;
		}
	}

	if (nbDummy || nbNotCoded) gf_import_message(import, GF_OK, "Removed Frames: %d VFW delay frames - %d N-VOPs", nbDummy, nbNotCoded);
	gf_isom_set_pl_indication(import->dest, GF_ISOM_PL_VISUAL, (u8) PL);

exit:
	if (destroy_esd) {
		gf_odf_desc_del((GF_Descriptor *) import->esd);
		import->esd = NULL;
	}
	if (frame) gf_free(frame);
	AVI_close(in);
	return e;
}

static GF_Err gf_import_avi_audio(GF_MediaImporter *import)
{
	GF_Err e;
	FILE *test;
	u64 duration;
	u32 hdr, di, track, i, tot_size;
	s64 offset;
	s32 size, max_size, done;
	u16 sampleRate;
	Double dur;
	Bool is_cbr;
	u8 oti;
	GF_ISOSample *samp;
	char *frame;
	Bool destroy_esd;
	s32 continuous;
	unsigned char temp[4];
	avi_t *in;

	/*video only, ignore*/
	if (import->trackID==1) return GF_OK;


	test = gf_fopen(import->in_name, "rb");
	if (!test) return gf_import_message(import, GF_URL_ERROR, "Opening file %s failed", import->in_name);
	gf_fclose(test);
	in = AVI_open_input_file(import->in_name, 1);
	if (!in) return gf_import_message(import, GF_NOT_SUPPORTED, "Unsupported avi file");

	AVI_seek_start(in);

	e = GF_OK;
	if (import->trackID)  AVI_set_audio_track(in, import->trackID-2);

	if (AVI_read_audio(in, (char *) temp, 4, &continuous) != 4) {
		AVI_close(in);
		return gf_import_message(import, GF_OK, "No audio track found");
	}

	hdr = GF_4CC(temp[0], temp[1], temp[2], temp[3]);
	if ((hdr &0xFFE00000) != 0xFFE00000) {
		AVI_close(in);
		return gf_import_message(import, GF_NOT_SUPPORTED, "Unsupported AVI audio format");
	}

	sampleRate = gf_mp3_sampling_rate(hdr);
	oti = gf_mp3_object_type_indication(hdr);
	if (!oti || !sampleRate) {
		AVI_close(in);
		return gf_import_message(import, GF_NOT_SUPPORTED, "Error: invalid MPEG Audio track");
	}

	frame = NULL;
	destroy_esd = GF_FALSE;
	if (!import->esd) {
		destroy_esd = GF_TRUE;
		import->esd = gf_odf_desc_esd_new(0);
	}
	track = gf_isom_new_track(import->dest, import->esd->ESID, GF_ISOM_MEDIA_AUDIO, sampleRate);
	if (!track) goto exit;
	gf_isom_set_track_enabled(import->dest, track, 1);
	if (!import->esd->ESID) import->esd->ESID = gf_isom_get_track_id(import->dest, track);
	import->final_trackID = import->esd->ESID;

	if (!import->esd->decoderConfig) import->esd->decoderConfig = (GF_DecoderConfig *) gf_odf_desc_new(GF_ODF_DCD_TAG);
	if (!import->esd->slConfig) import->esd->slConfig = (GF_SLConfig *) gf_odf_desc_new(GF_ODF_SLC_TAG);
	import->esd->slConfig->timestampResolution = sampleRate;
	if (import->esd->decoderConfig->decoderSpecificInfo) gf_odf_desc_del((GF_Descriptor *) import->esd->decoderConfig->decoderSpecificInfo);
	import->esd->decoderConfig->decoderSpecificInfo = NULL;
	import->esd->decoderConfig->streamType = GF_STREAM_AUDIO;
	import->esd->decoderConfig->objectTypeIndication = oti;
	e = gf_isom_new_mpeg4_description(import->dest, track, import->esd, (import->flags & GF_IMPORT_USE_DATAREF) ? import->in_name : NULL, NULL, &di);
	if (e) goto exit;

	gf_import_message(import, GF_OK, "AVI Audio import - sample rate %d - %s audio - %d channel%s", sampleRate, (oti==GPAC_OTI_AUDIO_MPEG1) ? "MPEG-1" : "MPEG-2", gf_mp3_num_channels(hdr), (gf_mp3_num_channels(hdr)>1) ? "s" : "");

	AVI_seek_start(in);
	AVI_set_audio_position(in, 0);

	i = 0;
	tot_size = max_size = 0;
	while ((size = (s32) AVI_audio_size(in, i) )>0) {
		if (max_size<size) max_size=size;
		tot_size += size;
		i++;
	}

	frame = (char*)gf_malloc(sizeof(char) * max_size);
	AVI_seek_start(in);
	AVI_set_audio_position(in, 0);

	dur = import->duration;
	dur *= sampleRate;
	dur /= 1000;
	duration = (u32) dur;

	samp = gf_isom_sample_new();
	done=max_size=0;
	is_cbr = GF_TRUE;
	while (1) {
		if (AVI_read_audio(in, frame, 4, (int*)&continuous) != 4) break;
		offset = gf_ftell(in->fdes) - 4;
		hdr = GF_4CC((u8) frame[0], (u8) frame[1], (u8) frame[2], (u8) frame[3]);

		size = gf_mp3_frame_size(hdr);
		if (size>max_size) {
			frame = (char*)gf_realloc(frame, sizeof(char) * size);
			if (max_size) is_cbr = GF_FALSE;
			max_size = size;
		}
		size = 4 + (s32) AVI_read_audio(in, &frame[4], size - 4, &continuous);

		if ((import->flags & GF_IMPORT_USE_DATAREF) && !continuous) {
			gf_import_message(import, GF_IO_ERR, "Cannot use media references, splitted input audio frame found");
			e = GF_IO_ERR;
			goto exit;
		}
		samp->IsRAP = RAP;
		samp->data = frame;
		samp->dataLength = size;
		if (import->flags & GF_IMPORT_USE_DATAREF) {
			e = gf_isom_add_sample_reference(import->dest, track, di, samp, offset);
		} else {
			e = gf_isom_add_sample(import->dest, track, di, samp);
		}
		if (e) goto exit;

		samp->DTS += gf_mp3_window_size(hdr);
		gf_set_progress("Importing AVI Audio", done, tot_size);

		done += size;
		if (duration && (samp->DTS > duration) ) break;
		if (import->flags & GF_IMPORT_DO_ABORT) break;
	}

	gf_set_progress("Importing AVI Audio", tot_size, tot_size);

	gf_import_message(import, GF_OK, "Import done - %s bit rate MP3 detected", is_cbr ? "constant" : "variable");
	samp->data = NULL;
	gf_isom_sample_del(&samp);

	gf_media_update_bitrate(import->dest, track);

	gf_isom_set_pl_indication(import->dest, GF_ISOM_PL_AUDIO, 0xFE);


exit:
	if (import->esd && destroy_esd) {
		gf_odf_desc_del((GF_Descriptor *) import->esd);
		import->esd = NULL;
	}
	if (frame) gf_free(frame);
	AVI_close(in);
	return e;
}
#endif /*GPAC_DISABLE_AVILIB*/


GF_Err gf_import_isomedia(GF_MediaImporter *import)
{
	GF_Err e;
	u64 offset, sampDTS, duration, dts_offset;
	u32 track, di, trackID, track_in, i, num_samples, mtype, w, h, sr, sbr_sr, ch, mstype, cur_extract_mode;
	s32 trans_x, trans_y;
	s16 layer;
	u8 bps;
	char *lang;
	const char *orig_name = gf_url_get_resource_name(gf_isom_get_filename(import->orig));
	Bool sbr, ps;
	GF_ISOSample *samp;
	GF_ESD *origin_esd;
	GF_InitialObjectDescriptor *iod;
	Bool is_cenc;
	sampDTS = 0;
	if (import->flags & GF_IMPORT_PROBE_ONLY) {
		for (i=0; i<gf_isom_get_track_count(import->orig); i++) {
			u32 mtype;
			import->tk_info[i].track_num = gf_isom_get_track_id(import->orig, i+1);
			mtype = gf_isom_get_media_type(import->orig, i+1);
			switch (mtype) {
			case GF_ISOM_MEDIA_VISUAL:
				import->tk_info[i].stream_type = GF_STREAM_VISUAL;
				break;
			case GF_ISOM_MEDIA_AUDIO:
				import->tk_info[i].stream_type = GF_STREAM_AUDIO;
				break;
			case GF_ISOM_MEDIA_TEXT:
				import->tk_info[i].stream_type = GF_STREAM_TEXT;
				break;
			case GF_ISOM_MEDIA_SCENE:
				import->tk_info[i].stream_type = GF_STREAM_SCENE;
				break;
			default:
				import->tk_info[i].stream_type = mtype;
				break;
			}
			import->tk_info[i].flags = GF_IMPORT_USE_DATAREF;
			if (import->tk_info[i].stream_type == GF_STREAM_VISUAL) {
				gf_isom_get_visual_info(import->orig, i+1, 1, &import->tk_info[i].video_info.width, &import->tk_info[i].video_info.height);
			} else if (import->tk_info[i].stream_type == GF_STREAM_AUDIO) {
				gf_isom_get_audio_info(import->orig, i+1, 1, &import->tk_info[i].audio_info.sample_rate, &import->tk_info[i].audio_info.nb_channels, NULL);
			}
			lang = NULL;
			gf_isom_get_media_language(import->orig, i+1, &lang);
			if (lang) {
				import->tk_info[i].lang = GF_4CC(' ', lang[0], lang[1], lang[2]);
				gf_free(lang);
				lang = NULL;
			}
			gf_media_get_rfc_6381_codec_name(import->orig, i+1, import->tk_info[i].szCodecProfile, GF_FALSE, GF_FALSE);

			import->nb_tracks ++;
		}
		return GF_OK;
	}

	trackID = import->trackID;
	if (!trackID) {
		if (gf_isom_get_track_count(import->orig) != 1) return gf_import_message(import, GF_BAD_PARAM, "Several tracks in MP4 - please indicate track to import");
		trackID = gf_isom_get_track_id(import->orig, 1);
	}
	track_in = gf_isom_get_track_by_id(import->orig, trackID);
	if (!track_in) return gf_import_message(import, GF_URL_ERROR, "Cannot find track ID %d in file", trackID);

	origin_esd = gf_isom_get_esd(import->orig, track_in, 1);

	if (import->esd && origin_esd) {
		origin_esd->OCRESID = import->esd->OCRESID;
		/*there may be other things to import...*/
	}
	ps = GF_FALSE;
	sbr = GF_FALSE;
	sbr_sr = 0;
	cur_extract_mode = gf_isom_get_nalu_extract_mode(import->orig, track_in);
	iod = (GF_InitialObjectDescriptor *) gf_isom_get_root_od(import->orig);
	if (iod && (iod->tag != GF_ODF_IOD_TAG)) {
		gf_odf_desc_del((GF_Descriptor *) iod);
		iod = NULL;
	}
	mtype = gf_isom_get_media_type(import->orig, track_in);
	if (mtype==GF_ISOM_MEDIA_VISUAL) {
		u8 PL = iod ? iod->visual_profileAndLevel : 0xFE;
		w = h = 0;
		gf_isom_get_visual_info(import->orig, track_in, 1, &w, &h);
#ifndef GPAC_DISABLE_AV_PARSERS
		/*for MPEG-4 visual, always check size (don't trust input file)*/
		if (origin_esd && (origin_esd->decoderConfig->objectTypeIndication==GPAC_OTI_VIDEO_MPEG4_PART2)) {
			GF_M4VDecSpecInfo dsi;
			gf_m4v_get_config(origin_esd->decoderConfig->decoderSpecificInfo->data, origin_esd->decoderConfig->decoderSpecificInfo->dataLength, &dsi);
			w = dsi.width;
			h = dsi.height;
			PL = dsi.VideoPL;
		}
#endif
		gf_isom_set_pl_indication(import->dest, GF_ISOM_PL_VISUAL, PL);
	}
	else if (mtype==GF_ISOM_MEDIA_AUDIO) {
		u8 PL = iod ? iod->audio_profileAndLevel : 0xFE;
		bps = 16;
		sr = ch = sbr_sr = 0;
		sbr = GF_FALSE;
		ps = GF_FALSE;
		gf_isom_get_audio_info(import->orig, track_in, 1, &sr, &ch, &bps);
#ifndef GPAC_DISABLE_AV_PARSERS
		if (origin_esd && (origin_esd->decoderConfig->objectTypeIndication==GPAC_OTI_AUDIO_AAC_MPEG4)) {
			if (origin_esd->decoderConfig->decoderSpecificInfo) {
				GF_M4ADecSpecInfo dsi;
				gf_m4a_get_config(origin_esd->decoderConfig->decoderSpecificInfo->data, origin_esd->decoderConfig->decoderSpecificInfo->dataLength, &dsi);
				sr = dsi.base_sr;
				if (dsi.has_sbr) sbr_sr = dsi.sbr_sr;
				ch = dsi.nb_chan;
				PL = dsi.audioPL;
				sbr = dsi.has_sbr ? ((dsi.base_object_type==GF_M4A_AAC_SBR || dsi.base_object_type==GF_M4A_AAC_PS) ? 2 : 1) : GF_FALSE;
				ps = dsi.has_ps;
			} else {
				GF_LOG(GF_LOG_WARNING, GF_LOG_PARSER, ("Missing DecoderSpecificInfo in MPEG-4 AAC stream\n"));
			}
		}
#endif
		gf_isom_set_pl_indication(import->dest, GF_ISOM_PL_AUDIO, PL);
	}
	else if (mtype==GF_ISOM_MEDIA_SUBPIC) {
		w = h = 0;
		trans_x = trans_y = 0;
		layer = 0;
		if (origin_esd && origin_esd->decoderConfig->objectTypeIndication == GPAC_OTI_MEDIA_SUBPIC) {
			gf_isom_get_track_layout_info(import->orig, track_in, &w, &h, &trans_x, &trans_y, &layer);
		}
	}

	gf_odf_desc_del((GF_Descriptor *) iod);
	if ( ! gf_isom_get_track_count(import->dest)) {
		u32 timescale = gf_isom_get_timescale(import->orig);
		gf_isom_set_timescale(import->dest, timescale);
	}

	e = gf_isom_clone_track(import->orig, track_in, import->dest, (import->flags & GF_IMPORT_USE_DATAREF) ? GF_TRUE : GF_FALSE, &track);
	if (e) goto exit;

	di = 1;

	if (import->esd && import->esd->ESID) {
		e = gf_isom_set_track_id(import->dest, track, import->esd->ESID);
		if (e) goto exit;
	}

	import->final_trackID = gf_isom_get_track_id(import->dest, track);
	if (import->esd && import->esd->dependsOnESID) {
		gf_isom_set_track_reference(import->dest, track, GF_ISOM_REF_DECODE, import->esd->dependsOnESID);
	}
	if (import->trackID && !(import->flags & GF_IMPORT_KEEP_REFS)) {
		gf_isom_remove_track_references(import->dest, track);
	}

	mstype = gf_isom_get_media_subtype(import->orig, track_in, di);

	switch (mtype) {
	case GF_ISOM_MEDIA_VISUAL:
		gf_import_message(import, GF_OK, "IsoMedia import %s - track ID %d - Video (size %d x %d)", orig_name, trackID, w, h);
		break;
	case GF_ISOM_MEDIA_AUDIO:
	{
		if (ps) {
			gf_import_message(import, GF_OK, "IsoMedia import %s - track ID %d - HE-AACv2 (SR %d - SBR-SR %d - %d channels)", orig_name, trackID, sr, sbr_sr, ch);
		} else if (sbr) {
			gf_import_message(import, GF_OK, "IsoMedia import %s - track ID %d - HE-AAC (SR %d - SBR-SR %d - %d channels)", orig_name, trackID, sr, sbr_sr, ch);
		} else {
			gf_import_message(import, GF_OK, "IsoMedia import %s - track ID %d - Audio (SR %d - %d channels)", orig_name, trackID, sr, ch);
		}
	}
	break;
	case GF_ISOM_MEDIA_SUBPIC:
		gf_import_message(import, GF_OK, "IsoMedia import %s - track ID %d - VobSub (size %d x %d)", orig_name, trackID, w, h);
		break;
	default:
	{
		char szT[5];
		mstype = gf_isom_get_mpeg4_subtype(import->orig, track_in, di);
		if (!mstype) mstype = gf_isom_get_media_subtype(import->orig, track_in, di);
		strcpy(szT, gf_4cc_to_str(mtype));
		gf_import_message(import, GF_OK, "IsoMedia import %s - track ID %d - media type \"%s:%s\"", orig_name, trackID, szT, gf_4cc_to_str(mstype));
	}
	break;
	}

	//this may happen with fragmented files
	dts_offset = 0;
	samp = gf_isom_get_sample_info(import->orig, track_in, 1, &di, &offset);
	if (samp) {
		dts_offset = samp->DTS;
		gf_isom_sample_del(&samp);
	}

	is_cenc = gf_isom_is_cenc_media(import->orig, track_in, 1);

	duration = (u64) (((Double)import->duration * gf_isom_get_media_timescale(import->orig, track_in)) / 1000);
	gf_isom_set_nalu_extract_mode(import->orig, track_in, GF_ISOM_NALU_EXTRACT_INSPECT);

	if (import->flags & GF_IMPORT_FORCE_XPS_INBAND) {
		if (is_cenc ) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_PARSER, ("[ISOM import] CENC media detected - cannot switch parameter set storage mode\n"));
		} else if (import->flags & GF_IMPORT_USE_DATAREF) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_PARSER, ("[ISOM import] Cannot switch parameter set storage mode when using data reference\n"));
		} else {
			switch (mstype) {
			case GF_ISOM_SUBTYPE_AVC_H264:
				gf_isom_set_nalu_extract_mode(import->orig, track_in, GF_ISOM_NALU_EXTRACT_INSPECT | GF_ISOM_NALU_EXTRACT_INBAND_PS_FLAG);
				gf_isom_avc_set_inband_config(import->dest, track, 1);
				break;
			case GF_ISOM_SUBTYPE_HVC1:
				gf_isom_set_nalu_extract_mode(import->orig, track_in, GF_ISOM_NALU_EXTRACT_INSPECT | GF_ISOM_NALU_EXTRACT_INBAND_PS_FLAG);
				gf_isom_hevc_set_inband_config(import->dest, track, 1);
				break;
			}
		}
	}

	num_samples = gf_isom_get_sample_count(import->orig, track_in);

	if (is_cenc) {
		u32 container_type;
		e = gf_isom_cenc_get_sample_aux_info(import->orig, track_in, 0, NULL, &container_type);
		if (e)
			goto exit;
		e = gf_isom_cenc_allocate_storage(import->dest, track, container_type, 0, 0, NULL);
		if (e) goto exit;
		e = gf_isom_clone_pssh(import->dest, import->orig, GF_FALSE);
		if (e) goto exit;
	}
	for (i=0; i<num_samples; i++) {
		if (import->flags & GF_IMPORT_USE_DATAREF) {
			samp = gf_isom_get_sample_info(import->orig, track_in, i+1, &di, &offset);
			if (!samp) {
				e = gf_isom_last_error(import->orig);
				goto exit;
			}
			samp->DTS -= dts_offset;
			e = gf_isom_add_sample_reference(import->dest, track, di, samp, offset);
		} else {
			samp = gf_isom_get_sample(import->orig, track_in, i+1, &di);
			if (!samp) {
				/*couldn't get the sample, but still move on*/
				goto exit;
			}
			samp->DTS -= dts_offset;
			/*if not first sample and same DTS as previous sample, force DTS++*/
			if (i && (samp->DTS<=sampDTS)) {
				if (i+1 < num_samples) {
					GF_LOG(GF_LOG_WARNING, GF_LOG_PARSER, ("[ISOM import] 0-duration sample detected at DTS %u - adjusting\n", samp->DTS));
				}
				samp->DTS = sampDTS + 1;
			}
			e = gf_isom_add_sample(import->dest, track, di, samp);
		}
		sampDTS = samp->DTS;
		gf_isom_sample_del(&samp);

		gf_isom_copy_sample_info(import->dest, track, import->orig, track_in, i+1);

		gf_set_progress("Importing ISO File", i+1, num_samples);


		if (duration && (sampDTS > duration) ) break;
		if (import->flags & GF_IMPORT_DO_ABORT) break;
		if (e)
			goto exit;
		if (is_cenc) {
			GF_CENCSampleAuxInfo *sai;
			u32 container_type, len, j, Is_Encrypted;
			u8 IV_size;
			bin128 KID;
			u8 crypt_byte_block, skip_byte_block;
			u8 constant_IV_size;
			bin128 constant_IV;
			GF_BitStream *bs;
			char *buffer;

			sai = NULL;
			e = gf_isom_cenc_get_sample_aux_info(import->orig, track_in, i+1, &sai, &container_type);
			if (e)
				goto exit;

			e = gf_isom_get_sample_cenc_info(import->orig, track_in, i+1, &Is_Encrypted, &IV_size, &KID, &crypt_byte_block, &skip_byte_block, &constant_IV_size, &constant_IV);
			if (e) goto exit;

			if (Is_Encrypted) {
				bs = gf_bs_new(NULL, 0, GF_BITSTREAM_WRITE);
				gf_bs_write_data(bs, (const char *)sai->IV, IV_size);
				if (sai->subsample_count) {
					gf_bs_write_u16(bs, sai->subsample_count);
					for (j = 0; j < sai->subsample_count; j++) {
						gf_bs_write_u16(bs, sai->subsamples[j].bytes_clear_data);
						gf_bs_write_u32(bs, sai->subsamples[j].bytes_encrypted_data);
					}
				}
				gf_isom_cenc_samp_aux_info_del(sai);
				gf_bs_get_content(bs, &buffer, &len);
				gf_bs_del(bs);
				e = gf_isom_track_cenc_add_sample_info(import->dest, track, container_type, IV_size, buffer, len);
				gf_free(buffer);
			} else {
				e = gf_isom_track_cenc_add_sample_info(import->dest, track, container_type, IV_size, NULL, 0);
			}
			if (e) goto exit;

			e = gf_isom_set_sample_cenc_group(import->dest, track, i+1, Is_Encrypted, IV_size, KID, crypt_byte_block, skip_byte_block, constant_IV_size, constant_IV);
			if (e) goto exit;
		}
	}

	//adjust last sample duration
	if (i==num_samples) {
		u32 dur = gf_isom_get_sample_duration(import->orig, track_in, num_samples);
		gf_isom_set_last_sample_duration(import->dest, track, dur);
	} else {
		s64 mediaOffset;
		if (gf_isom_get_edit_list_type(import->orig, track_in, &mediaOffset)) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_AUTHOR, ("[ISOBMF Import] Multiple edits found in source media, import may be broken\n"));
		}
		gf_isom_update_edit_list_duration(import->dest, track);
		gf_isom_update_duration(import->dest);
	}

	if (gf_isom_has_time_offset(import->orig, track_in)==2) {
		e = gf_isom_set_composition_offset_mode(import->dest, track, GF_TRUE);
		if (e)
			goto exit;
	}


	if (import->esd) {
		if (!import->esd->slConfig) {
			import->esd->slConfig = origin_esd ? origin_esd->slConfig : NULL;
			if (origin_esd) origin_esd->slConfig = NULL;
		}
		if (!import->esd->decoderConfig) {
			import->esd->decoderConfig = origin_esd ? origin_esd->decoderConfig : NULL;
			if (origin_esd) origin_esd->decoderConfig = NULL;
		}
	}

	gf_media_update_bitrate(import->dest, track);

exit:
	if (origin_esd) gf_odf_desc_del((GF_Descriptor *) origin_esd);
	gf_isom_set_nalu_extract_mode(import->orig, track_in, cur_extract_mode);
	return e;
}

#ifndef GPAC_DISABLE_MPEG2PS

#include "mpeg2_ps.h"

static GF_Err gf_import_mpeg_ps_video(GF_MediaImporter *import)
{
	GF_Err e;
	mpeg2ps_t *ps;
	Double FPS;
	char *buf;
	u8 ftype;
	u32 track, di, streamID, mtype, w, h, ar, nb_streams, buf_len, frames, ref_frame, timescale, dts_inc, last_pos;
	u64 file_size, duration;
	Bool destroy_esd;

	if (import->flags & GF_IMPORT_USE_DATAREF)
		return gf_import_message(import, GF_NOT_SUPPORTED, "Cannot use data referencing with MPEG-1/2 files");

	/*no auto frame-rate detection*/
	if (import->video_fps == GF_IMPORT_AUTO_FPS)
		import->video_fps = GF_IMPORT_DEFAULT_FPS;

	ps = mpeg2ps_init(import->in_name);
	if (!ps) return gf_import_message(import, GF_NON_COMPLIANT_BITSTREAM, "Failed to open MPEG file %s", import->in_name);

	if (import->flags & GF_IMPORT_PROBE_ONLY) {
		u32 i, nb_v_str;
		import->nb_tracks = 0;
		nb_v_str = nb_streams = mpeg2ps_get_video_stream_count(ps);
		for (i=0; i<nb_streams; i++) {
			import->tk_info[import->nb_tracks].track_num = i+1;
			import->tk_info[import->nb_tracks].stream_type = GF_STREAM_VISUAL;
			import->tk_info[import->nb_tracks].flags = GF_IMPORT_OVERRIDE_FPS;

			import->tk_info[import->nb_tracks].video_info.FPS = mpeg2ps_get_video_stream_framerate(ps, i);
			import->tk_info[import->nb_tracks].video_info.width = mpeg2ps_get_video_stream_width(ps, i);
			import->tk_info[import->nb_tracks].video_info.height = mpeg2ps_get_video_stream_height(ps, i);
			import->tk_info[import->nb_tracks].video_info.par = mpeg2ps_get_video_stream_aspect_ratio(ps, i);

			import->tk_info[import->nb_tracks].media_oti = GPAC_OTI_VIDEO_MPEG1;
			if (mpeg2ps_get_video_stream_type(ps, i) == MPEG_VIDEO_MPEG2)
				import->tk_info[import->nb_tracks].media_oti = GPAC_OTI_VIDEO_MPEG2_MAIN;

			import->nb_tracks++;
		}
		nb_streams = mpeg2ps_get_audio_stream_count(ps);
		for (i=0; i<nb_streams; i++) {
			import->tk_info[import->nb_tracks].track_num = nb_v_str + i+1;
			import->tk_info[import->nb_tracks].stream_type = GF_STREAM_AUDIO;
			switch (mpeg2ps_get_audio_stream_type(ps, i)) {
			case MPEG_AUDIO_MPEG:
				import->tk_info[import->nb_tracks].media_oti = GPAC_OTI_AUDIO_MPEG1;
				break;
			case MPEG_AUDIO_AC3:
				import->tk_info[import->nb_tracks].media_oti = GPAC_OTI_AUDIO_AC3;
				break;
			default:
				import->tk_info[import->nb_tracks].media_4cc = GF_MEDIA_TYPE_UNK;
				break;
			}
			import->tk_info[import->nb_tracks].audio_info.sample_rate = mpeg2ps_get_audio_stream_sample_freq(ps, i);
			import->tk_info[import->nb_tracks].audio_info.nb_channels = mpeg2ps_get_audio_stream_channels(ps, i);
			import->nb_tracks ++;
		}
		mpeg2ps_close(ps);
		return GF_OK;
	}


	streamID = 0;
	nb_streams = mpeg2ps_get_video_stream_count(ps);
	if ((nb_streams>1) && !import->trackID) {
		mpeg2ps_close(ps);
		return gf_import_message(import, GF_BAD_PARAM, "%d video tracks in MPEG file - please indicate track to import", nb_streams);
	}
	/*audio*/
	if (import->trackID>nb_streams) {
		mpeg2ps_close(ps);
		return GF_OK;
	}
	if (import->trackID) streamID = import->trackID - 1;

	if (streamID>=nb_streams) {
		mpeg2ps_close(ps);
		return gf_import_message(import, GF_BAD_PARAM, "Desired video track not found in MPEG file (%d visual streams)", nb_streams);
	}
	w = mpeg2ps_get_video_stream_width(ps, streamID);
	h = mpeg2ps_get_video_stream_height(ps, streamID);
	ar = mpeg2ps_get_video_stream_aspect_ratio(ps, streamID);
	mtype = (mpeg2ps_get_video_stream_type(ps, streamID) == MPEG_VIDEO_MPEG2) ? GPAC_OTI_VIDEO_MPEG2_MAIN : GPAC_OTI_VIDEO_MPEG1;
	FPS = mpeg2ps_get_video_stream_framerate(ps, streamID);
	if (import->video_fps) FPS = (Double) import->video_fps;
	get_video_timing(FPS, &timescale, &dts_inc);

	duration = import->duration;
	duration *= timescale;
	duration /= 1000;

	destroy_esd = GF_FALSE;
	if (!import->esd) {
		destroy_esd = GF_TRUE;
		import->esd = gf_odf_desc_esd_new(0);
	}
	track = gf_isom_new_track(import->dest, import->esd->ESID, GF_ISOM_MEDIA_VISUAL, timescale);
	e = gf_isom_last_error(import->dest);
	if (!track) goto exit;
	gf_isom_set_track_enabled(import->dest, track, 1);
	if (!import->esd->ESID) import->esd->ESID = gf_isom_get_track_id(import->dest, track);
	import->final_trackID = import->esd->ESID;

	if (!import->esd->decoderConfig) import->esd->decoderConfig = (GF_DecoderConfig *) gf_odf_desc_new(GF_ODF_DCD_TAG);
	if (!import->esd->slConfig) import->esd->slConfig = (GF_SLConfig *) gf_odf_desc_new(GF_ODF_SLC_TAG);
	import->esd->slConfig->timestampResolution = timescale;
	if (import->esd->decoderConfig->decoderSpecificInfo) gf_odf_desc_del((GF_Descriptor *) import->esd->decoderConfig->decoderSpecificInfo);
	import->esd->decoderConfig->decoderSpecificInfo = NULL;
	import->esd->decoderConfig->streamType = GF_STREAM_VISUAL;
	import->esd->decoderConfig->objectTypeIndication = mtype;
	e = gf_isom_new_mpeg4_description(import->dest, track, import->esd, NULL, NULL, &di);
	if (e) goto exit;

	gf_import_message(import, GF_OK, "%s Video import - Resolution %d x %d @ %02.4f FPS", (mtype==GPAC_OTI_VIDEO_MPEG1) ? "MPEG-1" : "MPEG-2", w, h, FPS);
	gf_isom_set_visual_info(import->dest, track, di, w, h);

	if (!gf_isom_get_media_timescale(import->dest, track)) {
		e = gf_import_message(import, GF_BAD_PARAM, "No timescale for imported track - ignoring");
		if (e) goto exit;
	}

	gf_isom_set_cts_packing(import->dest, track, GF_TRUE);

	file_size = mpeg2ps_get_ps_size(ps);
	last_pos = 0;
	frames = 1;
	ref_frame = 1;
	while (mpeg2ps_get_video_frame(ps, streamID, (u8 **) &buf, &buf_len, &ftype, TS_90000, NULL)) {
		GF_ISOSample *samp;
		if ((buf[buf_len - 4] == 0) && (buf[buf_len - 3] == 0) && (buf[buf_len - 2] == 1)) buf_len -= 4;
		samp = gf_isom_sample_new();
		samp->data = buf;
		samp->dataLength = buf_len;
		samp->DTS = (u64)dts_inc*(frames-1);
		samp->IsRAP = (ftype==1) ? RAP : RAP_NO;
		samp->CTS_Offset = 0;
		e = gf_isom_add_sample(import->dest, track, di, samp);
		samp->data = NULL;
		gf_isom_sample_del(&samp);
		if (e) goto exit;

		last_pos = (u32) mpeg2ps_get_video_pos(ps, streamID);
		gf_set_progress("Importing MPEG-PS Video", last_pos/1024, file_size/1024);

		if (ftype != 3) {
			gf_isom_modify_cts_offset(import->dest, track, ref_frame, (frames-ref_frame)*dts_inc);
			ref_frame = frames;
		}
		frames++;

		if (duration && (dts_inc*(frames-1) >= duration) ) break;
		if (import->flags & GF_IMPORT_DO_ABORT) break;
	}
	gf_isom_set_cts_packing(import->dest, track, GF_FALSE);
	if (!(import->flags & GF_IMPORT_NO_EDIT_LIST))
		update_edit_list_for_bframes(import->dest, track);

	if (last_pos!=file_size) gf_set_progress("Importing MPEG-PS Video", frames, frames);

	gf_media_update_bitrate(import->dest, track);
	if (ar) gf_media_change_par(import->dest, track, ar>>16, ar&0xffff);

exit:
	if (import->esd && destroy_esd) {
		gf_odf_desc_del((GF_Descriptor *) import->esd);
		import->esd = NULL;
	}
	mpeg2ps_close(ps);
	return e;
}

static GF_Err gf_import_mpeg_ps_audio(GF_MediaImporter *import)
{
	GF_Err e;
	mpeg2ps_t *ps;
	char *buf;
	u32 track, di, streamID, mtype, sr, nb_ch, nb_streams, buf_len, frames, hdr, last_pos;
	u64 file_size, duration;
	Bool destroy_esd;
	GF_ISOSample *samp;

	if (import->flags & GF_IMPORT_PROBE_ONLY) return GF_OK;

	if (import->flags & GF_IMPORT_USE_DATAREF)
		return gf_import_message(import, GF_NOT_SUPPORTED, "Cannot use data referencing with MPEG-1/2 files");

	ps = mpeg2ps_init(import->in_name);
	if (!ps) return gf_import_message(import, GF_NON_COMPLIANT_BITSTREAM, "Failed to open MPEG file %s", import->in_name);


	streamID = 0;
	nb_streams = mpeg2ps_get_audio_stream_count(ps);
	if ((nb_streams>1) && !import->trackID) {
		mpeg2ps_close(ps);
		return gf_import_message(import, GF_BAD_PARAM, "%d audio tracks in MPEG file - please indicate track to import", nb_streams);
	}

	if (import->trackID) {
		u32 nb_v = mpeg2ps_get_video_stream_count(ps);
		/*video*/
		if (import->trackID<=nb_v) {
			mpeg2ps_close(ps);
			return GF_OK;
		}
		streamID = import->trackID - 1 - nb_v;
	}

	if (streamID>=nb_streams) {
		mpeg2ps_close(ps);
		return gf_import_message(import, GF_BAD_PARAM, "Desired audio track not found in MPEG file (%d audio streams)", nb_streams);
	}

	mtype = mpeg2ps_get_audio_stream_type(ps, streamID);
	if (mtype != MPEG_AUDIO_MPEG) {
		mpeg2ps_close(ps);
		return gf_import_message(import, GF_NOT_SUPPORTED, "Audio format not supported in MP4");
	}

	if (mpeg2ps_get_audio_frame(ps, streamID, (u8**) &buf, &buf_len, TS_90000, NULL, NULL) == 0) {
		mpeg2ps_close(ps);
		return gf_import_message(import, GF_IO_ERR, "Cannot fetch audio frame from MPEG file");
	}

	hdr = GF_4CC((u8)buf[0],(u8)buf[1],(u8)buf[2],(u8)buf[3]);
	mtype = gf_mp3_object_type_indication(hdr);
	sr = gf_mp3_sampling_rate(hdr);
	nb_ch = gf_mp3_num_channels(hdr);

	destroy_esd = GF_FALSE;
	if (!import->esd) {
		destroy_esd = GF_TRUE;
		import->esd = gf_odf_desc_esd_new(0);
	}
	track = gf_isom_new_track(import->dest, import->esd->ESID, GF_ISOM_MEDIA_AUDIO, sr);
	e = gf_isom_last_error(import->dest);
	if (!track) goto exit;
	gf_isom_set_track_enabled(import->dest, track, 1);
	if (!import->esd->ESID) import->esd->ESID = gf_isom_get_track_id(import->dest, track);
	import->final_trackID = import->esd->ESID;

	if (!import->esd->decoderConfig) import->esd->decoderConfig = (GF_DecoderConfig *) gf_odf_desc_new(GF_ODF_DCD_TAG);
	if (!import->esd->slConfig) import->esd->slConfig = (GF_SLConfig *) gf_odf_desc_new(GF_ODF_SLC_TAG);
	import->esd->slConfig->timestampResolution = sr;
	if (import->esd->decoderConfig->decoderSpecificInfo) gf_odf_desc_del((GF_Descriptor *) import->esd->decoderConfig->decoderSpecificInfo);
	import->esd->decoderConfig->decoderSpecificInfo = NULL;
	import->esd->decoderConfig->streamType = GF_STREAM_AUDIO;
	import->esd->decoderConfig->objectTypeIndication = mtype;
	e = gf_isom_new_mpeg4_description(import->dest, track, import->esd, NULL, NULL, &di);
	if (e) goto exit;

	gf_isom_set_audio_info(import->dest, track, di, sr, nb_ch, 16);
	gf_import_message(import, GF_OK, "%s Audio import - sample rate %d - %d channel%s", (mtype==GPAC_OTI_AUDIO_MPEG1) ? "MPEG-1" : "MPEG-2", sr, nb_ch, (nb_ch>1) ? "s" : "");


	duration = (u64) ((Double)import->duration/1000.0 * sr);

	samp = gf_isom_sample_new();
	samp->IsRAP = RAP;
	samp->DTS = 0;

	file_size = mpeg2ps_get_ps_size(ps);
	frames = 0;
	do {
		samp->data = buf;
		samp->dataLength = buf_len;
		e = gf_isom_add_sample(import->dest, track, di, samp);
		if (e) goto exit;
		samp->DTS += gf_mp3_window_size(hdr);
		last_pos = (u32) mpeg2ps_get_audio_pos(ps, streamID);
		gf_set_progress("Importing MPEG-PS Audio", last_pos/1024, file_size/1024);
		frames++;
		if (duration && (samp->DTS>=duration)) break;
		if (import->flags & GF_IMPORT_DO_ABORT) break;
	}  while (mpeg2ps_get_audio_frame(ps, streamID, (u8**)&buf, &buf_len, TS_90000, NULL, NULL));

	samp->data = NULL;
	gf_isom_sample_del(&samp);
	if (last_pos!=file_size) gf_set_progress("Importing MPEG-PS Audio", frames, frames);
	gf_media_update_bitrate(import->dest, track);

exit:
	if (import->esd && destroy_esd) {
		gf_odf_desc_del((GF_Descriptor *) import->esd);
		import->esd = NULL;
	}
	mpeg2ps_close(ps);
	return e;
}
#endif /*GPAC_DISABLE_MPEG2PS*/






GF_EXPORT
GF_Err gf_media_avc_rewrite_samples(GF_ISOFile *file, u32 track, u32 prev_size, u32 new_size)
{
	u32 i, count, di, remain, msize;
	char *buffer;

	msize = 4096;
	buffer = (char*)gf_malloc(sizeof(char)*msize);
	count = gf_isom_get_sample_count(file, track);
	for (i=0; i<count; i++) {
		GF_ISOSample *samp = gf_isom_get_sample(file, track, i+1, &di);
		GF_BitStream *oldbs = gf_bs_new(samp->data, samp->dataLength, GF_BITSTREAM_READ);
		GF_BitStream *newbs = gf_bs_new(NULL, 0, GF_BITSTREAM_WRITE);
		remain = samp->dataLength;
		while (remain) {
			u32 size = gf_bs_read_int(oldbs, prev_size);
			gf_bs_write_int(newbs, size, new_size);
			remain -= prev_size/8;
			if (size>msize) {
				msize = size;
				buffer = (char*)gf_realloc(buffer, sizeof(char)*msize);
			}
			gf_bs_read_data(oldbs, buffer, size);
			gf_bs_write_data(newbs, buffer, size);
			remain -= size;
		}
		gf_bs_del(oldbs);
		gf_free(samp->data);
		samp->data = NULL;
		samp->dataLength = 0;
		gf_bs_get_content(newbs, &samp->data, &samp->dataLength);
		gf_bs_del(newbs);
		gf_isom_update_sample(file, track, i+1, samp, GF_TRUE);
		gf_isom_sample_del(&samp);
	}
	gf_free(buffer);
	return GF_OK;
}

#ifndef GPAC_DISABLE_AV_PARSERS

static GF_Err gf_import_avc_h264(GF_MediaImporter *import)
{
	u64 nal_start, nal_end, total_size;
	u32 nal_size, track, trackID, di, cur_samp, nb_i, nb_idr, nb_p, nb_b, nb_sp, nb_si, nb_sei, max_w, max_h, max_total_delay, nb_nalus;
	s32 idx, sei_recovery_frame_count;
	u64 duration;
	u8 nal_type;
	GF_Err e;
	FILE *mdia;
	AVCState avc;
	GF_AVCConfigSlot *slc;
	GF_AVCConfig *avccfg, *svccfg, *dstcfg;
	GF_BitStream *bs;
	GF_BitStream *sample_data;
	Bool flush_sample, sample_is_rap, sample_has_islice, sample_has_slice, is_islice, first_nal, slice_is_ref, has_cts_offset, detect_fps, is_paff, set_subsamples, slice_force_ref;
	u32 ref_frame, timescale, copy_size, size_length, dts_inc;
	s32 last_poc, max_last_poc, max_last_b_poc, poc_diff, prev_last_poc, min_poc, poc_shift;
	Bool first_avc;
	u32 use_opengop_gdr = 0;
	u32 last_svc_sps;
	u32 prev_nalu_prefix_size, res_prev_nalu_prefix;
	u8 priority_prev_nalu_prefix;
	Double FPS;
	char *buffer;
	u32 max_size = 4096;

	if (import->flags & GF_IMPORT_PROBE_ONLY) {
		import->nb_tracks = 1;
		import->tk_info[0].track_num = 1;
		import->tk_info[0].stream_type = GF_STREAM_VISUAL;
		import->tk_info[0].flags = GF_IMPORT_OVERRIDE_FPS | GF_IMPORT_FORCE_PACKED;
		return GF_OK;
	}

	set_subsamples = (import->flags & GF_IMPORT_SET_SUBSAMPLES) ? GF_TRUE : GF_FALSE;

	mdia = gf_fopen(import->in_name, "rb");
	if (!mdia) return gf_import_message(import, GF_URL_ERROR, "Cannot find file %s", import->in_name);

	detect_fps = GF_TRUE;
	FPS = (Double) import->video_fps;
	if (!FPS) {
		FPS = GF_IMPORT_DEFAULT_FPS;
	} else {
		if (import->video_fps == GF_IMPORT_AUTO_FPS)
			import->video_fps = GF_IMPORT_DEFAULT_FPS;	/*fps=auto is handled as auto-detection is h264*/
		else
			detect_fps = GF_FALSE;								/*fps is forced by the caller*/
	}
	get_video_timing(FPS, &timescale, &dts_inc);

	poc_diff = 0;


restart_import:

	memset(&avc, 0, sizeof(AVCState));
	avc.sps_active_idx = -1;
	avccfg = gf_odf_avc_cfg_new();
	svccfg = gf_odf_avc_cfg_new();
	/*we don't handle split import (one track / layer)*/
	svccfg->complete_representation = 1;
	buffer = (char*)gf_malloc(sizeof(char) * max_size);
	sample_data = NULL;
	first_avc = GF_TRUE;
	last_svc_sps = 0;
	sei_recovery_frame_count = -1;

	bs = gf_bs_from_file(mdia, GF_BITSTREAM_READ);
	if (!gf_media_nalu_is_start_code(bs)) {
		e = gf_import_message(import, GF_NON_COMPLIANT_BITSTREAM, "Cannot find H264 start code");
		goto exit;
	}

	/*NALU size packing disabled*/
	if (!(import->flags & GF_IMPORT_FORCE_PACKED)) size_length = 32;
	/*if import in edit mode, use smallest NAL size and adjust on the fly*/
	else if (gf_isom_get_mode(import->dest)!=GF_ISOM_OPEN_WRITE) size_length = 8;
	else size_length = 32;

	trackID = 0;

	if (import->esd) trackID = import->esd->ESID;

	track = gf_isom_new_track(import->dest, trackID, GF_ISOM_MEDIA_VISUAL, timescale);
	if (!track) {
		e = gf_isom_last_error(import->dest);
		goto exit;
	}
	gf_isom_set_track_enabled(import->dest, track, 1);
	if (import->esd && !import->esd->ESID) import->esd->ESID = gf_isom_get_track_id(import->dest, track);
	import->final_trackID = gf_isom_get_track_id(import->dest, track);
	if (import->esd && import->esd->dependsOnESID) {
		gf_isom_set_track_reference(import->dest, track, GF_ISOM_REF_DECODE, import->esd->dependsOnESID);
	}

	e = gf_isom_avc_config_new(import->dest, track, avccfg, NULL, NULL, &di);
	if (e) goto exit;

	gf_isom_set_nalu_extract_mode(import->dest, track, GF_ISOM_NALU_EXTRACT_INSPECT);

	sample_data = NULL;
	sample_is_rap = GF_FALSE;
	sample_has_islice = GF_FALSE;
	sample_has_slice = GF_FALSE;
	cur_samp = 0;
	is_paff = GF_FALSE;
	total_size = gf_bs_get_size(bs);
	nal_start = gf_bs_get_position(bs);
	duration = (u64) ( ((Double)import->duration) * timescale / 1000.0);

	nb_i = nb_idr = nb_p = nb_b = nb_sp = nb_si = nb_sei = 0;
	max_w = max_h = 0;
	first_nal = GF_TRUE;
	ref_frame = 0;
	last_poc = max_last_poc = max_last_b_poc = prev_last_poc = 0;
	max_total_delay = 0;

	gf_isom_set_cts_packing(import->dest, track, GF_TRUE);
	has_cts_offset = GF_FALSE;
	min_poc = 0;
	poc_shift = 0;
	prev_nalu_prefix_size = 0;
	res_prev_nalu_prefix = 0;
	priority_prev_nalu_prefix = 0;
	nb_nalus = 0;

	while (gf_bs_available(bs)) {
		u8 nal_hdr, skip_nal, is_subseq, add_sps;
		u32 nal_and_trailing_size;

		nal_and_trailing_size = nal_size = gf_media_nalu_next_start_code_bs(bs);
		if (!(import->flags & GF_IMPORT_KEEP_TRAILING)) {
			nal_size = gf_media_nalu_payload_end_bs(bs);
		}

		if (nal_size>max_size) {
			buffer = (char*)gf_realloc(buffer, sizeof(char)*nal_size);
			max_size = nal_size;
		}

		/*read the file, and work on a memory buffer*/
		gf_bs_read_data(bs, buffer, nal_size);

		gf_bs_seek(bs, nal_start);
		nal_hdr = gf_bs_read_u8(bs);
		nal_type = nal_hdr & 0x1F;

		is_subseq = 0;
		skip_nal = 0;
		copy_size = flush_sample = GF_FALSE;
		is_islice = GF_FALSE;

		if (nal_type == GF_AVC_NALU_SVC_SUBSEQ_PARAM || nal_type == GF_AVC_NALU_SVC_PREFIX_NALU || nal_type == GF_AVC_NALU_SVC_SLICE) {
			avc.is_svc = GF_TRUE;
		}
		nb_nalus ++;

		switch (gf_media_avc_parse_nalu(bs, nal_hdr, &avc)) {
		case 1:
			if (import->flags & GF_IMPORT_FORCE_XPS_INBAND) {
				if (sample_has_slice) flush_sample = GF_TRUE;
			} else {
				flush_sample = GF_TRUE;
			}
			break;
		case -1:
			gf_import_message(import, GF_OK, "Warning: Error parsing NAL unit");
			skip_nal = 1;
			break;
		case -2:
			skip_nal = 1;
			break;
		default:
			break;
		}
		switch (nal_type) {
		case GF_AVC_NALU_SVC_SUBSEQ_PARAM:
			is_subseq = 1;
		case GF_AVC_NALU_SEQ_PARAM:
			idx = gf_media_avc_read_sps(buffer, nal_size, &avc, is_subseq, NULL);
			if (idx<0) {
				if (avc.sps[0].profile_idc) {
					GF_LOG(GF_LOG_WARNING, GF_LOG_CONTAINER, ("Error parsing SeqInfo"));
				} else {
					e = gf_import_message(import, GF_NON_COMPLIANT_BITSTREAM, "Error parsing SeqInfo");
					goto exit;
				}
			}
			add_sps = 0;
			dstcfg = (import->flags & GF_IMPORT_SVC_EXPLICIT) ? svccfg : avccfg;
			if (is_subseq) {
				if ((avc.sps[idx].state & AVC_SUBSPS_PARSED) && !(avc.sps[idx].state & AVC_SUBSPS_DECLARED)) {
					avc.sps[idx].state |= AVC_SUBSPS_DECLARED;
					add_sps = 1;
				}
				dstcfg = svccfg;
				if (import->flags & GF_IMPORT_SVC_NONE) {
					add_sps = 0;
				}
			} else {
				if ((avc.sps[idx].state & AVC_SPS_PARSED) && !(avc.sps[idx].state & AVC_SPS_DECLARED)) {
					avc.sps[idx].state |= AVC_SPS_DECLARED;
					add_sps = 1;
				}
			}
			/*some streams are not really nice and reuse sps idx with differnet parameters (typically
			when concatenated bitstreams). Since we cannot put two SPS with the same idx in the decoder config, we keep them in the
			video bitstream*/
			if (avc.sps[idx].state & AVC_SUBSPS_DECLARED) {
				if (import->flags & GF_IMPORT_SVC_NONE) {
					copy_size = 0;
				} else {
					copy_size = nal_size;
				}
			}

			//always keep NAL
			if (import->flags & GF_IMPORT_FORCE_XPS_INBAND) {
				copy_size = nal_size;
				if (sample_has_slice) flush_sample = GF_TRUE;
			}

			//first declaration of SPS,
			if (add_sps) {
				dstcfg->configurationVersion = 1;
				dstcfg->profile_compatibility = avc.sps[idx].prof_compat;
				dstcfg->AVCProfileIndication = avc.sps[idx].profile_idc;
				dstcfg->AVCLevelIndication = avc.sps[idx].level_idc;

				dstcfg->chroma_format = avc.sps[idx].chroma_format;
				dstcfg->luma_bit_depth = 8 + avc.sps[idx].luma_bit_depth_m8;
				dstcfg->chroma_bit_depth = 8 + avc.sps[idx].chroma_bit_depth_m8;
				/*try to patch ?*/
				if (!gf_avc_is_rext_profile(dstcfg->AVCProfileIndication)
					&& ((dstcfg->chroma_format>1) || (dstcfg->luma_bit_depth>8) || (dstcfg->chroma_bit_depth>8))
				) {
					if ((dstcfg->luma_bit_depth>8) || (dstcfg->chroma_bit_depth>8)) {
						dstcfg->AVCProfileIndication=110;
					} else {
						dstcfg->AVCProfileIndication = (dstcfg->chroma_format==3) ? 244 : 122;
					}
				}

				if (import->flags & GF_IMPORT_FORCE_XPS_INBAND) {
					copy_size = nal_size;
				} else {
					slc = (GF_AVCConfigSlot*)gf_malloc(sizeof(GF_AVCConfigSlot));
					slc->size = nal_size;
					slc->id = idx;
					slc->data = (char*)gf_malloc(sizeof(char)*slc->size);
					memcpy(slc->data, buffer, sizeof(char)*slc->size);
					gf_list_add(dstcfg->sequenceParameterSets, slc);
				}

				/*disable frame rate scan, most bitstreams have wrong values there*/
				if (detect_fps && avc.sps[idx].vui.timing_info_present_flag
				        /*if detected FPS is greater than 1000, assume wrong timing info*/
				        && (avc.sps[idx].vui.time_scale <= 1000*avc.sps[idx].vui.num_units_in_tick)
				   ) {
					/*ISO/IEC 14496-10 n11084 Table E-6*/
					/* not used :				u8 DeltaTfiDivisorTable[] = {1,1,1,2,2,2,2,3,3,4,6}; */
					u8 DeltaTfiDivisorIdx;
					if (!avc.sps[idx].vui.pic_struct_present_flag) {
						DeltaTfiDivisorIdx = 1 + (1-avc.s_info.field_pic_flag);
					} else {
						if (!avc.sei.pic_timing.pic_struct)
							DeltaTfiDivisorIdx = 2;
						else if (avc.sei.pic_timing.pic_struct == 8)
							DeltaTfiDivisorIdx = 6;
						else
							DeltaTfiDivisorIdx = (avc.sei.pic_timing.pic_struct+1) / 2;
					}
					timescale = 2 * avc.sps[idx].vui.time_scale;
					dts_inc =   2 * avc.sps[idx].vui.num_units_in_tick * DeltaTfiDivisorIdx;
					FPS = (Double)timescale / dts_inc;
					detect_fps = GF_FALSE;

					if (!avc.sps[idx].vui.fixed_frame_rate_flag)
						GF_LOG(GF_LOG_INFO, GF_LOG_CODING, ("[avc-h264] Possible Variable Frame Rate: VUI \"fixed_frame_rate_flag\" absent.\n"));

					gf_isom_remove_track(import->dest, track);
					if (sample_data) gf_bs_del(sample_data);
					gf_odf_avc_cfg_del(avccfg);
					avccfg = NULL;
					gf_odf_avc_cfg_del(svccfg);
					svccfg = NULL;
					gf_free(buffer);
					buffer = NULL;
					gf_bs_del(bs);
					bs = NULL;
					gf_fseek(mdia, 0, SEEK_SET);
					goto restart_import;
				}

				if (is_subseq) {
					if (last_svc_sps<(u32) idx) {
						if (import->flags & GF_IMPORT_SVC_EXPLICIT) {
							gf_import_message(import, GF_OK, "SVC-H264 import - frame size %d x %d at %02.3f FPS", avc.sps[idx].width, avc.sps[idx].height, FPS);
						} else {
							gf_import_message(import, GF_OK, "SVC Detected - SSPS ID %d - frame size %d x %d", idx-GF_SVC_SSPS_ID_SHIFT, avc.sps[idx].width, avc.sps[idx].height);
						}
						last_svc_sps = idx;
					}
					/* prevent from adding the subseq PS to the samples */
					copy_size = 0;
				} else {
					if (first_avc) {
						first_avc = GF_FALSE;
						if (!(import->flags & GF_IMPORT_SVC_EXPLICIT)) {
							gf_import_message(import, GF_OK, "AVC-H264 import - frame size %d x %d at %02.3f FPS", avc.sps[idx].width, avc.sps[idx].height, FPS);
						}
					}
				}
				if (!is_subseq || (import->flags & GF_IMPORT_SVC_EXPLICIT)) {
					if ((max_w <= avc.sps[idx].width) && (max_h <= avc.sps[idx].height)) {
						max_w = avc.sps[idx].width;
						max_h = avc.sps[idx].height;
					}
				}
			}
			break;
		case GF_AVC_NALU_PIC_PARAM:
			idx = gf_media_avc_read_pps(buffer, nal_size, &avc);
			if (idx<0) {
				e = gf_import_message(import, GF_NON_COMPLIANT_BITSTREAM, "Error parsing Picture Param");
				goto exit;
			}
			/*some streams are not really nice and reuse sps idx with differnet parameters (typically
			when concatenated bitstreams). Since we cannot put two SPS with the same idx in the decoder config, we keep them in the
			video bitstream*/
			if (avc.pps[idx].status == 2) {
				copy_size = nal_size;
			}

			//always keep NAL
			if (import->flags & GF_IMPORT_FORCE_XPS_INBAND) {
				copy_size = nal_size;
				if (sample_has_slice) flush_sample = GF_TRUE;
			} else {
				if (avc.pps[idx].status==1) {
					avc.pps[idx].status = 2;
					slc = (GF_AVCConfigSlot*)gf_malloc(sizeof(GF_AVCConfigSlot));
					slc->size = nal_size;
					slc->id = idx;
					slc->data = (char*)gf_malloc(sizeof(char)*slc->size);
					memcpy(slc->data, buffer, sizeof(char)*slc->size);

					/* by default, we put all PPS in the base AVC layer,
					  they will be moved to the SVC layer upon analysis of SVC slice. */
					//dstcfg = (import->flags & GF_IMPORT_SVC_EXPLICIT) ? svccfg : avccfg;
					dstcfg = avccfg;

					if (import->flags & GF_IMPORT_SVC_EXPLICIT)
						dstcfg = svccfg;

					gf_list_add(dstcfg->pictureParameterSets, slc);
				}
			}
			break;
		case GF_AVC_NALU_SEI:
			if (import->flags & GF_IMPORT_NO_SEI) {
				copy_size = 0;
			} else {
				if (avc.sps_active_idx != -1) {
					copy_size = gf_media_avc_reformat_sei(buffer, nal_size, &avc);
					if (copy_size)
						nb_sei++;
				}
			}
			break;

		case GF_AVC_NALU_NON_IDR_SLICE:
		case GF_AVC_NALU_DP_A_SLICE:
		case GF_AVC_NALU_DP_B_SLICE:
		case GF_AVC_NALU_DP_C_SLICE:
		case GF_AVC_NALU_IDR_SLICE:
			if (! skip_nal) {
				copy_size = nal_size;
				switch (avc.s_info.slice_type) {
				case GF_AVC_TYPE_P:
				case GF_AVC_TYPE2_P:
					nb_p++;
					break;
				case GF_AVC_TYPE_I:
				case GF_AVC_TYPE2_I:
					nb_i++;
					is_islice = GF_TRUE;
					break;
				case GF_AVC_TYPE_B:
				case GF_AVC_TYPE2_B:
					nb_b++;
					break;
				case GF_AVC_TYPE_SP:
				case GF_AVC_TYPE2_SP:
					nb_sp++;
					break;
				case GF_AVC_TYPE_SI:
				case GF_AVC_TYPE2_SI:
					nb_si++;
					break;
				}
			}
			break;

		/*remove*/
		case GF_AVC_NALU_ACCESS_UNIT:
		case GF_AVC_NALU_FILLER_DATA:
		case GF_AVC_NALU_END_OF_SEQ:
		case GF_AVC_NALU_END_OF_STREAM:
			break;

		case GF_AVC_NALU_SVC_PREFIX_NALU:
			if (import->flags & GF_IMPORT_SVC_NONE) {
				copy_size = 0;
				break;
			}
			copy_size = nal_size;
			break;
		case GF_AVC_NALU_SVC_SLICE:
		{
			u32 i;
			for (i = 0; i < gf_list_count(avccfg->pictureParameterSets); i ++) {
				slc = (GF_AVCConfigSlot*)gf_list_get(avccfg->pictureParameterSets, i);
				if (avc.s_info.pps->id == slc->id) {
					/* This PPS is used by an SVC NAL unit, it should be moved to the SVC Config Record) */
					gf_list_rem(avccfg->pictureParameterSets, i);
					i--;
					gf_list_add(svccfg->pictureParameterSets, slc);
				}
			}
		}
		if (import->flags & GF_IMPORT_SVC_NONE) {
			copy_size = 0;
			break;
		}
		if (! skip_nal) {
			copy_size = nal_size;
			switch (avc.s_info.slice_type) {
			case GF_AVC_TYPE_P:
			case GF_AVC_TYPE2_P:
				avc.s_info.sps->nb_ep++;
				break;
			case GF_AVC_TYPE_I:
			case GF_AVC_TYPE2_I:
				avc.s_info.sps->nb_ei++;
				break;
			case GF_AVC_TYPE_B:
			case GF_AVC_TYPE2_B:
				avc.s_info.sps->nb_eb++;
				break;
			}
		}
		break;

		case GF_AVC_NALU_SEQ_PARAM_EXT:
			idx = gf_media_avc_read_sps_ext(buffer, nal_size);
			if (idx<0) {
				e = gf_import_message(import, GF_NON_COMPLIANT_BITSTREAM, "Error parsing Sequence Param Extension");
				goto exit;
			}

			if (! (avc.sps[idx].state & AVC_SPS_EXT_DECLARED)) {
				avc.sps[idx].state |= AVC_SPS_EXT_DECLARED;

				slc = (GF_AVCConfigSlot*)gf_malloc(sizeof(GF_AVCConfigSlot));
				slc->size = nal_size;
				slc->id = idx;
				slc->data = (char*)gf_malloc(sizeof(char)*slc->size);
				memcpy(slc->data, buffer, sizeof(char)*slc->size);

				if (!avccfg->sequenceParameterSetExtensions)
					avccfg->sequenceParameterSetExtensions = gf_list_new();

				gf_list_add(avccfg->sequenceParameterSetExtensions, slc);
			}
			break;

		case GF_AVC_NALU_SLICE_AUX:

		default:
			gf_import_message(import, GF_OK, "WARNING: NAL Unit type %d not handled - adding", nal_type);
			copy_size = nal_size;
			break;
		}

		if (!nal_size) break;

		if (flush_sample && sample_data) {
			GF_ISOSample *samp = gf_isom_sample_new();
			samp->DTS = (u64)dts_inc*cur_samp;
			samp->IsRAP = sample_is_rap ? RAP : RAP_NO;
			if (!sample_is_rap) {
				if (sample_has_islice && (import->flags & GF_IMPORT_FORCE_SYNC) && (sei_recovery_frame_count==0)) {
					samp->IsRAP = RAP;
					if (!use_opengop_gdr) {
						use_opengop_gdr = 1;
						GF_LOG(GF_LOG_WARNING, GF_LOG_CODING, ("[AVC Import] Forcing non-IDR samples with I slices to be marked as sync points - resulting file will not be ISO conformant\n"));
					}
				}
			}
			gf_bs_get_content(sample_data, &samp->data, &samp->dataLength);
			gf_bs_del(sample_data);
			sample_data = NULL;

			if (prev_nalu_prefix_size) {
				u32 size, reserved, nb_subs;
				u8 priority;
				Bool discardable;

				samp->dataLength -= size_length/8 + prev_nalu_prefix_size;

				if (set_subsamples) {
					/* determine the number of subsamples */
					nb_subs = gf_isom_sample_has_subsamples(import->dest, track, cur_samp+1, 0);
					if (nb_subs) {
						/* fetch size, priority, reserved and discardable info for last subsample */
						gf_isom_sample_get_subsample(import->dest, track, cur_samp+1, 0, nb_subs, &size, &priority, &reserved, &discardable);

						/*remove last subsample entry!*/
						gf_isom_add_subsample(import->dest, track, cur_samp+1, 0, 0, 0, 0, GF_FALSE);
					}
				}

				/*rewrite last NALU prefix at the beginning of next sample*/
				sample_data = gf_bs_new(NULL, 0, GF_BITSTREAM_WRITE);
				gf_bs_write_data(sample_data, samp->data + samp->dataLength, size_length/8 + prev_nalu_prefix_size);

				if (set_subsamples) {
					/*add subsample entry to next sample*/
					gf_isom_add_subsample(import->dest, track, cur_samp+2, 0, size_length/8 + prev_nalu_prefix_size, priority, reserved, discardable);
				}

				prev_nalu_prefix_size = 0;
			}
			/*CTS recomuting is much trickier than with MPEG-4 ASP due to b-slice used as references - we therefore
			store the POC as the CTS offset and update the whole table at the end*/
			samp->CTS_Offset = last_poc - poc_shift;
			assert(last_poc >= poc_shift);
			e = gf_isom_add_sample(import->dest, track, di, samp);
			if (e) goto exit;

			sample_has_slice = GF_FALSE;
			cur_samp++;

			/*write sampleGroups info*/
			if (!samp->IsRAP && ( (sei_recovery_frame_count>=0) || sample_has_islice) ) {
				/*generic GDR*/
				if (sei_recovery_frame_count > 0) {
					if (!use_opengop_gdr) use_opengop_gdr = 1;
					e = gf_isom_set_sample_roll_group(import->dest, track, cur_samp, (s16) sei_recovery_frame_count);
				}
				/*open-GOP*/
				else if ((sei_recovery_frame_count == 0) && sample_has_islice) {
					if (!use_opengop_gdr) use_opengop_gdr = 2;
					e = gf_isom_set_sample_rap_group(import->dest, track, cur_samp, 0);
				}
				if (e) goto exit;
			}

			gf_isom_sample_del(&samp);
			gf_set_progress("Importing AVC-H264", (u32) (nal_start/1024), (u32) (total_size/1024) );
			first_nal = GF_TRUE;

			if (min_poc > last_poc)
				min_poc = last_poc;

			sample_has_islice = GF_FALSE;
			sei_recovery_frame_count = -1;
		}

		if (copy_size) {
			if (is_islice)
				sample_has_islice = GF_TRUE;

			if ((size_length<32) && ( (u32) (1<<size_length)-1 < copy_size)) {
				u32 diff_size = 8;
				while ((size_length<32) && ( (u32) (1<<(size_length+diff_size))-1 < copy_size)) diff_size+=8;
				/*only 8bits, 16bits and 32 bits*/
				if (size_length+diff_size == 24) diff_size+=8;

				gf_import_message(import, GF_OK, "Adjusting AVC SizeLength to %d bits", size_length+diff_size);
				gf_media_avc_rewrite_samples(import->dest, track, size_length, size_length+diff_size);

				/*rewrite current sample*/
				if (sample_data) {
					char *sd;
					u32 sd_l;
					GF_BitStream *prev_sd;
					gf_bs_get_content(sample_data, &sd, &sd_l);
					gf_bs_del(sample_data);
					sample_data = gf_bs_new(NULL, 0, GF_BITSTREAM_WRITE);
					prev_sd = gf_bs_new(sd, sd_l, GF_BITSTREAM_READ);
					while (gf_bs_available(prev_sd)) {
						char *buf;
						u32 s = gf_bs_read_int(prev_sd, size_length);
						gf_bs_write_int(sample_data, s, size_length+diff_size);
						buf = (char*)gf_malloc(sizeof(char)*s);
						gf_bs_read_data(prev_sd, buf, s);
						gf_bs_write_data(sample_data, buf, s);
						gf_free(buf);
					}
					gf_bs_del(prev_sd);
					gf_free(sd);
				}
				size_length+=diff_size;

			}
			if (!sample_data) sample_data = gf_bs_new(NULL, 0, GF_BITSTREAM_WRITE);
			gf_bs_write_int(sample_data, copy_size, size_length);
			gf_bs_write_data(sample_data, buffer, copy_size);

			/*fixme - we need finer grain for priority*/
			if ((nal_type==GF_AVC_NALU_SVC_PREFIX_NALU) || (nal_type==GF_AVC_NALU_SVC_SLICE)) {
				u32 res = 0;
				u8 prio;
				unsigned char *p = (unsigned char *) buffer;
				res |= (p[0] & 0x60) ? 0x80000000 : 0; // RefPicFlag
				res |= (0) ? 0x40000000 : 0;             // RedPicFlag TODO: not supported, would require to parse NAL unit payload
				res |= (1<=nal_type && nal_type<=5) || (nal_type==GF_AVC_NALU_SVC_PREFIX_NALU) || (nal_type==GF_AVC_NALU_SVC_SLICE) ? 0x20000000 : 0;  // VclNALUnitFlag
				res |= p[1] << 16;                     // use values of IdrFlag and PriorityId directly from SVC extension header
				res |= p[2] << 8;                      // use values of DependencyId and QualityId directly from SVC extension header
				res |= p[3] & 0xFC;                    // use values of TemporalId and UseRefBasePicFlag directly from SVC extension header
				res |= (0) ? 0x00000002 : 0;             // StoreBaseRepFlag TODO: SVC FF mentions a store_base_rep_flag which cannot be found in SVC spec

				// priority_id (6 bits) in SVC has inverse meaning -> lower value means higher priority - invert it and scale it to 8 bits
				prio = (63 - (p[1] & 0x3F)) << 2;

				if (set_subsamples) {
					gf_isom_add_subsample(import->dest, track, cur_samp+1, 0, copy_size+size_length/8, prio, res, GF_TRUE);
				}

				if (nal_type==GF_AVC_NALU_SVC_PREFIX_NALU) {
					/* remember reserved and priority value */
					res_prev_nalu_prefix = res;
					priority_prev_nalu_prefix = prio;
				}
			} else if (set_subsamples) {
				/* use the res and priority value of last prefix NALU */
				gf_isom_add_subsample(import->dest, track, cur_samp+1, 0, copy_size+size_length/8, priority_prev_nalu_prefix, res_prev_nalu_prefix, GF_FALSE);
			}
			if (nal_type!=GF_AVC_NALU_SVC_PREFIX_NALU) {
				res_prev_nalu_prefix = 0;
				priority_prev_nalu_prefix = 0;
			}

			if (nal_type != GF_AVC_NALU_SVC_PREFIX_NALU) {
				prev_nalu_prefix_size = 0;
			} else {
				prev_nalu_prefix_size += nal_size;
			}

			switch (nal_type) {
			case GF_AVC_NALU_SVC_SLICE:
			case GF_AVC_NALU_NON_IDR_SLICE:
			case GF_AVC_NALU_DP_A_SLICE:
			case GF_AVC_NALU_DP_B_SLICE:
			case GF_AVC_NALU_DP_C_SLICE:
			case GF_AVC_NALU_IDR_SLICE:
			case GF_AVC_NALU_SLICE_AUX:
				sample_has_slice = GF_TRUE;
				if (!is_paff && avc.s_info.bottom_field_flag)
					is_paff = GF_TRUE;

				slice_is_ref = (avc.s_info.nal_unit_type==GF_AVC_NALU_IDR_SLICE) ? GF_TRUE : GF_FALSE;
				if (slice_is_ref)
					nb_idr++;
				slice_force_ref = GF_FALSE;

				/*we only indicate TRUE IDRs for sync samples (cf AVC file format spec).
				SEI recovery should be used to build sampleToGroup & RollRecovery tables*/
				if (first_nal) {
					first_nal = GF_FALSE;
					if (avc.sei.recovery_point.valid || (import->flags & GF_IMPORT_FORCE_SYNC)) {
						Bool bIntraSlice = gf_media_avc_slice_is_intra(&avc);
						assert(avc.s_info.nal_unit_type!=GF_AVC_NALU_IDR_SLICE || bIntraSlice);

						sei_recovery_frame_count = avc.sei.recovery_point.frame_cnt;

						/*we allow to mark I-frames as sync on open-GOPs (with sei_recovery_frame_count=0) when forcing sync even when the SEI RP is not available*/
						if (!avc.sei.recovery_point.valid && bIntraSlice) {
							sei_recovery_frame_count = 0;
							if (use_opengop_gdr == 1) {
								use_opengop_gdr = 2; /*avoid message flooding*/
								GF_LOG(GF_LOG_WARNING, GF_LOG_CODING, ("[AVC Import] No valid SEI Recovery Point found although needed - forcing\n"));
							}
						}
						avc.sei.recovery_point.valid = 0;
						if (bIntraSlice && (import->flags & GF_IMPORT_FORCE_SYNC) && (sei_recovery_frame_count==0))
							slice_force_ref = GF_TRUE;
					}
					sample_is_rap = gf_media_avc_slice_is_IDR(&avc);
				}

				if (avc.s_info.poc<poc_shift) {
					u32 j;
					if (ref_frame) {
						for (j=ref_frame; j<=cur_samp; j++) {
							GF_ISOSample *samp = gf_isom_get_sample_info(import->dest, track, j, NULL, NULL);
							if (!samp) break;
							samp->CTS_Offset += poc_shift;
							samp->CTS_Offset -= avc.s_info.poc;
							gf_isom_modify_cts_offset(import->dest, track, j, samp->CTS_Offset);
							gf_isom_sample_del(&samp);
						}
					}
					poc_shift = avc.s_info.poc;
				}

				/*if #pics, compute smallest POC increase*/
				if (avc.s_info.poc != last_poc) {
					if (!poc_diff || (poc_diff > abs(avc.s_info.poc-last_poc))) {
						poc_diff = abs(avc.s_info.poc-last_poc);/*ideally we would need to start the parsing again as poc_diff helps computing max_total_delay*/
					}
					last_poc = avc.s_info.poc;
				}

				/*ref slice, reset poc*/
				if (slice_is_ref) {
					ref_frame = cur_samp+1;
					max_last_poc = last_poc = max_last_b_poc = 0;
					poc_shift = 0;
				}
				/*forced ref slice*/
				else if (slice_force_ref) {
					ref_frame = cur_samp+1;
					/*adjust POC shift as sample will now be marked as sync, so wo must store poc as if IDR (eg POC=0) for our CTS offset computing to be correct*/
					poc_shift = avc.s_info.poc;
				}
				/*strictly less - this is a new P slice*/
				else if (max_last_poc<last_poc) {
					max_last_b_poc = 0;
					//prev_last_poc = max_last_poc;
					max_last_poc = last_poc;
				}
				/*stricly greater*/
				else if (max_last_poc>last_poc) {
					/*need to store TS offsets*/
					has_cts_offset = GF_TRUE;
					switch (avc.s_info.slice_type) {
					case GF_AVC_TYPE_B:
					case GF_AVC_TYPE2_B:
						if (!max_last_b_poc) {
							max_last_b_poc = last_poc;
						}
						/*if same poc than last max, this is a B-slice*/
						else if (last_poc>max_last_b_poc) {
							max_last_b_poc = last_poc;
						}
						/*otherwise we had a B-slice reference: do nothing*/

						break;
					}
				}

				/*compute max delay (applicable when B slice are present)*/
				if (ref_frame && poc_diff && (s32)(cur_samp-(ref_frame-1)-last_poc/poc_diff)>(s32)max_total_delay) {
					max_total_delay = cur_samp - (ref_frame-1) - last_poc/poc_diff;
				}
			}
		}

		gf_bs_align(bs);
		nal_end = gf_bs_get_position(bs);
		assert(nal_start <= nal_end);
		assert(nal_end <= nal_start + nal_and_trailing_size);
		if (nal_end != nal_start + nal_and_trailing_size)
			gf_bs_seek(bs, nal_start + nal_and_trailing_size);

		if (!gf_bs_available(bs)) break;
		if (duration && (dts_inc*cur_samp > duration)) break;
		if (import->flags & GF_IMPORT_DO_ABORT) break;

		/*consume next start code*/
		nal_start = gf_media_nalu_next_start_code_bs(bs);
		if (nal_start) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODING, ("[avc-h264] invalid nal_size (%u)? Skipping "LLU" bytes to reach next start code\n", nal_size, nal_start));
			gf_bs_skip_bytes(bs, nal_start);
		}
		nal_start = gf_media_nalu_is_start_code(bs);
		if (!nal_start) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODING, ("[avc-h264] error: no start code found ("LLU" bytes read out of "LLU") - leaving\n", gf_bs_get_position(bs), gf_bs_get_size(bs)));
			break;
		}
		nal_start = gf_bs_get_position(bs);
	}

	/*final flush*/
	if (sample_data) {
		GF_ISOSample *samp = gf_isom_sample_new();
		samp->DTS = (u64)dts_inc*cur_samp;
		samp->IsRAP = sample_is_rap ? RAP : RAP_NO;
		if (!sample_is_rap && sample_has_islice && (import->flags & GF_IMPORT_FORCE_SYNC)) {
			samp->IsRAP = RAP;
		}
		/*we store the frame order (based on the POC) as the CTS offset and update the whole table at the end*/
		samp->CTS_Offset = last_poc - poc_shift;
		gf_bs_get_content(sample_data, &samp->data, &samp->dataLength);
		gf_bs_del(sample_data);
		sample_data = NULL;
		e = gf_isom_add_sample(import->dest, track, di, samp);
		if (e) goto exit;
		gf_isom_sample_del(&samp);
		gf_set_progress("Importing AVC-H264", (u32) cur_samp, cur_samp+1);
		cur_samp++;
	}


	/*recompute all CTS offsets*/
	if (has_cts_offset) {
		u32 i, last_cts_samp;
		u64 last_dts, max_cts, min_cts, min_cts_offset;
		if (!poc_diff) poc_diff = 1;
		/*no b-frame references, no need to cope with negative poc*/
		if (!max_total_delay) {
			min_poc=0;
			max_total_delay = 1;
		}
		cur_samp = gf_isom_get_sample_count(import->dest, track);
		min_poc *= -1;
		last_dts = 0;
		max_cts = 0;
		min_cts = (u64) -1;
		min_cts_offset = (u64) -1;
		last_cts_samp = 0;

		for (i=0; i<cur_samp; i++) {
			u64 cts;
			/*not using descIdx and data_offset will only fecth DTS, CTS and RAP which is all we need*/
			GF_ISOSample *samp = gf_isom_get_sample_info(import->dest, track, i+1, NULL, NULL);
			/*poc re-init (RAP and POC to 0, otherwise that's SEI recovery), update base DTS*/
			if (samp->IsRAP /*&& !samp->CTS_Offset*/)
				last_dts = samp->DTS * (1+is_paff);

			/*CTS offset is frame POC (refers to last IDR)*/
			cts = ( (min_poc + (s32) samp->CTS_Offset) * dts_inc ) / poc_diff + (u32) last_dts;

			/*if PAFF, 2 pictures (eg poc) <=> 1 aggregated frame (eg sample), divide by 2*/
			if (is_paff) {
				cts /= 2;
				/*in some cases the poc is not on the top field - if that is the case, round up*/
				if (cts%dts_inc) {
					cts = ((cts/dts_inc)+1)*dts_inc;
				}
			}

			/*B-frames offset*/
			cts += (u32) (max_total_delay*dts_inc);

			samp->CTS_Offset = (u32) (cts - samp->DTS);

			if (samp->CTS_Offset < min_cts_offset)
				min_cts_offset = samp->CTS_Offset;

			if (max_cts < samp->DTS + samp->CTS_Offset) {
				max_cts = samp->DTS + samp->CTS_Offset;
				last_cts_samp = i;
			}
			if (min_cts >= samp->DTS + samp->CTS_Offset)
				min_cts = samp->DTS + samp->CTS_Offset;


			/*this should never happen, however some streams seem to do weird POC increases (cf sorenson streams, last 2 frames),
			this should hopefully take care of some bugs and ensure proper CTS...*/
			if ((s32)samp->CTS_Offset<0) {
				u32 j, k;
				samp->CTS_Offset = 0;
				gf_isom_modify_cts_offset(import->dest, track, i+1, samp->CTS_Offset);
				for (j=last_cts_samp; j<i; j++) {
					GF_ISOSample *asamp = gf_isom_get_sample_info(import->dest, track, j+1, NULL, NULL);
					for (k=j+1; k<=i; k++) {
						GF_ISOSample *bsamp = gf_isom_get_sample_info(import->dest, track, k+1, NULL, NULL);
						if (asamp->CTS_Offset+asamp->DTS==bsamp->CTS_Offset+bsamp->DTS) {
							max_cts += dts_inc;
							bsamp->CTS_Offset = (u32) (max_cts - bsamp->DTS);
							gf_isom_modify_cts_offset(import->dest, track, k+1, bsamp->CTS_Offset);
						}
						gf_isom_sample_del(&bsamp);
					}
					gf_isom_sample_del(&asamp);
				}
				max_cts = samp->DTS + samp->CTS_Offset;
			} else {
				gf_isom_modify_cts_offset(import->dest, track, i+1, samp->CTS_Offset);
			}
			gf_isom_sample_del(&samp);
		}

		if (min_cts_offset > 0) {
			gf_isom_shift_cts_offset(import->dest, track, (s32)min_cts_offset);
		}
		/*and repack table*/
		gf_isom_set_cts_packing(import->dest, track, GF_FALSE);

		if (!(import->flags & GF_IMPORT_NO_EDIT_LIST) && min_cts) {
			last_dts = max_cts - min_cts + gf_isom_get_sample_duration(import->dest, track, gf_isom_get_sample_count(import->dest, track) );

			last_dts *= gf_isom_get_timescale(import->dest);
			last_dts /= gf_isom_get_media_timescale(import->dest, track);
			gf_isom_set_edit_segment(import->dest, track, 0, last_dts, min_cts, GF_ISOM_EDIT_NORMAL);
		}
	} else {
		gf_isom_remove_cts_info(import->dest, track);
	}

	if (gf_isom_get_sample_count(import->dest,track) == 1) {
	    gf_isom_set_last_sample_duration(import->dest, track, dts_inc );
	}

	gf_set_progress("Importing AVC-H264", (u32) cur_samp, cur_samp);

	gf_isom_set_visual_info(import->dest, track, di, max_w, max_h);
	avccfg->nal_unit_size = size_length/8;
	svccfg->nal_unit_size = size_length/8;


	if (import->flags & GF_IMPORT_FORCE_XPS_INBAND) {
		gf_isom_avc_config_update(import->dest, track, 1, avccfg);
		gf_isom_avc_set_inband_config(import->dest, track, 1);
	} else if (gf_list_count(avccfg->sequenceParameterSets) || !gf_list_count(svccfg->sequenceParameterSets) ) {
		gf_isom_avc_config_update(import->dest, track, 1, avccfg);
		if (gf_list_count(svccfg->sequenceParameterSets)) {
			gf_isom_svc_config_update(import->dest, track, 1, svccfg, GF_TRUE);
		}
	} else {
		gf_isom_svc_config_update(import->dest, track, 1, svccfg, GF_FALSE);
	}


	gf_media_update_par(import->dest, track);
	gf_media_update_bitrate(import->dest, track);

	gf_isom_set_pl_indication(import->dest, GF_ISOM_PL_VISUAL, 0x7F);
	gf_isom_modify_alternate_brand(import->dest, GF_ISOM_BRAND_AVC1, 1);

	if (!gf_list_count(avccfg->sequenceParameterSets) && !gf_list_count(svccfg->sequenceParameterSets) && !(import->flags & GF_IMPORT_FORCE_XPS_INBAND)) {
		e = gf_import_message(import, GF_NON_COMPLIANT_BITSTREAM, "Import results: No SPS or PPS found in the bitstream ! Nothing imported\n");
	} else {
		u32 i;
		if (nb_sp || nb_si) {
			gf_import_message(import, GF_OK, "AVC Import results: %d samples (%d NALUs) - Slices: %d I %d P %d B %d SP %d SI - %d SEI - %d IDR",
			                  cur_samp, nb_nalus, nb_i, nb_p, nb_b, nb_sp, nb_si, nb_sei, nb_idr);
		} else {
			gf_import_message(import, GF_OK, "AVC Import results: %d samples (%d NALUs) - Slices: %d I %d P %d B - %d SEI - %d IDR",
			                  cur_samp, nb_nalus, nb_i, nb_p, nb_b, nb_sei, nb_idr);
		}

		for (i=0; i<gf_list_count(svccfg->sequenceParameterSets); i++) {
			AVC_SPS *sps;
			GF_AVCConfigSlot *svcc = (GF_AVCConfigSlot*)gf_list_get(svccfg->sequenceParameterSets, i);
			sps = & avc.sps[svcc->id];
			if (sps && (sps->state & AVC_SUBSPS_PARSED)) {
				gf_import_message(import, GF_OK, "SVC (SSPS ID %d) Import results: Slices: %d I %d P %d B", svcc->id - GF_SVC_SSPS_ID_SHIFT, sps->nb_ei, sps->nb_ep, sps->nb_eb);
			}
		}

		if (max_total_delay>1) {
			gf_import_message(import, GF_OK, "Stream uses forward prediction - stream CTS offset: %d frames", max_total_delay);
		}
	}

	if (use_opengop_gdr==2) {
		gf_import_message(import, GF_OK, "OpenGOP detected - adjusting file brand");
		gf_isom_modify_alternate_brand(import->dest, GF_ISOM_BRAND_ISO6, 1);
	}

	/*rewrite ESD*/
	if (import->esd) {
		if (!import->esd->slConfig) import->esd->slConfig = (GF_SLConfig*) gf_odf_desc_new(GF_ODF_SLC_TAG);
		import->esd->slConfig->predefined = 2;
		import->esd->slConfig->timestampResolution = timescale;
		if (import->esd->decoderConfig) gf_odf_desc_del((GF_Descriptor *)import->esd->decoderConfig);
		import->esd->decoderConfig = gf_isom_get_decoder_config(import->dest, track, 1);
		gf_isom_change_mpeg4_description(import->dest, track, 1, import->esd);
	}

exit:
	if (sample_data) gf_bs_del(sample_data);
	gf_odf_avc_cfg_del(avccfg);
	gf_odf_avc_cfg_del(svccfg);
	gf_free(buffer);
	gf_bs_del(bs);
	gf_fclose(mdia);
	return e;
}

#ifndef GPAC_DISABLE_HEVC
static GF_HEVCParamArray *get_hevc_param_array(GF_HEVCConfig *hevc_cfg, u8 type)
{
	u32 i, count = hevc_cfg->param_array ? gf_list_count(hevc_cfg->param_array) : 0;
	for (i=0; i<count; i++) {
		GF_HEVCParamArray *ar = (GF_HEVCParamArray*)gf_list_get(hevc_cfg->param_array, i);
		if (ar->type==type) return ar;
	}
	return NULL;
}


static void hevc_set_parall_type(GF_HEVCConfig *hevc_cfg)
{
	u32 use_tiles, use_wpp, nb_pps, i, count;
	HEVCState hevc;
	GF_HEVCParamArray *ar = get_hevc_param_array(hevc_cfg, GF_HEVC_NALU_PIC_PARAM);
	if (!ar)
		return;

	count = gf_list_count(ar->nalus);

	memset(&hevc, 0, sizeof(HEVCState));
	hevc.sps_active_idx = -1;

	use_tiles = 0;
	use_wpp = 0;
	nb_pps = 0;

	for (i=0; i<count; i++) {
		HEVC_PPS *pps;
		GF_AVCConfigSlot *slc = (GF_AVCConfigSlot*)gf_list_get(ar->nalus, i);
		s32 idx = gf_media_hevc_read_pps(slc->data, slc->size, &hevc);

		if (idx>=0) {
			nb_pps++;
			pps = &hevc.pps[idx];
			if (!pps->entropy_coding_sync_enabled_flag && pps->tiles_enabled_flag)
				use_tiles++;
			else if (pps->entropy_coding_sync_enabled_flag && !pps->tiles_enabled_flag)
				use_wpp++;
		}
	}
	if (!use_tiles && !use_wpp) hevc_cfg->parallelismType = 1;
	else if (!use_wpp && (use_tiles==nb_pps) ) hevc_cfg->parallelismType = 2;
	else if (!use_tiles && (use_wpp==nb_pps) ) hevc_cfg->parallelismType = 3;
	else hevc_cfg->parallelismType = 0;
}

#endif

static GF_Err gf_lhevc_set_operating_points_information(GF_ISOFile *file, u32 hevc_track, u32 track, HEVC_VPS *vps, u8 *max_temporal_id)
{
	GF_OperatingPointsInformation *oinf;
	u32 di = 0;
	GF_BitStream *bs;
	char *data;
	u32 data_size;
	u32 i;

	if (!vps->vps_extension_found) return GF_OK;

	oinf = gf_isom_oinf_new_entry();
	if (!oinf) return GF_OUT_OF_MEM;

	oinf->scalability_mask = 0;
	for (i = 0; i < 16; i++) {
		if (vps->scalability_mask[i])
			oinf->scalability_mask |= 1 << i;
	}

	for (i = 0; i < vps->num_profile_tier_level; i++) {
		HEVC_ProfileTierLevel ptl = (i == 0) ? vps->ptl : vps->ext_ptl[i-1];
		LHEVC_ProfileTierLevel *lhevc_ptl;
		GF_SAFEALLOC(lhevc_ptl, LHEVC_ProfileTierLevel);
		lhevc_ptl->general_profile_space = ptl.profile_space;
		lhevc_ptl->general_tier_flag = ptl.tier_flag;
		lhevc_ptl->general_profile_idc = ptl.profile_idc;
		lhevc_ptl->general_profile_compatibility_flags = ptl.profile_compatibility_flag;
		lhevc_ptl->general_constraint_indicator_flags = 0;
		if (ptl.general_progressive_source_flag)
			lhevc_ptl->general_constraint_indicator_flags |= ((u64)1) << 47;
		if (ptl.general_interlaced_source_flag)
			lhevc_ptl->general_constraint_indicator_flags |= ((u64)1) << 46;
		if (ptl.general_non_packed_constraint_flag)
			lhevc_ptl->general_constraint_indicator_flags |= ((u64)1) << 45;
		if (ptl.general_frame_only_constraint_flag)
			lhevc_ptl->general_constraint_indicator_flags |= ((u64)1) << 44;
		lhevc_ptl->general_constraint_indicator_flags |= ptl.general_reserved_44bits;
		lhevc_ptl->general_level_idc = ptl.level_idc;
		gf_list_add(oinf->profile_tier_levels, lhevc_ptl);
	}

	for (i = 0; i < vps->num_output_layer_sets; i++) {
		LHEVC_OperatingPoint *op;
		u32 j;
		u16 minPicWidth, minPicHeight, maxPicWidth, maxPicHeight;
		u8 maxChromaFormat, maxBitDepth;
		u8 maxTemporalId;
		GF_SAFEALLOC(op, LHEVC_OperatingPoint);
		op->output_layer_set_idx = i;
		op->layer_count = vps->num_necessary_layers[i];
		minPicWidth = minPicHeight = maxPicWidth = maxPicHeight = maxTemporalId = 0;
		maxChromaFormat = maxBitDepth = 0;
		for (j = 0; j < op->layer_count; j++) {
			u32 format_idx;
			u32 bitDepth;
			op->layers_info[j].ptl_idx = vps->profile_tier_level_idx[i][j];
			op->layers_info[j].layer_id = j;
			op->layers_info[j].is_outputlayer = vps->output_layer_flag[i][j];
			//FIXME: we consider that this flag is never set
			op->layers_info[j].is_alternate_outputlayer = GF_FALSE;
			if (!maxTemporalId || (maxTemporalId < max_temporal_id[op->layers_info[j].layer_id]))
				maxTemporalId = max_temporal_id[op->layers_info[j].layer_id];
			format_idx = vps->rep_format_idx[op->layers_info[j].layer_id];
			if (!minPicWidth || (minPicWidth > vps->rep_formats[format_idx].pic_width_luma_samples))
				minPicWidth = vps->rep_formats[format_idx].pic_width_luma_samples;
			if (!minPicHeight || (minPicHeight > vps->rep_formats[format_idx].pic_height_luma_samples))
				minPicHeight = vps->rep_formats[format_idx].pic_height_luma_samples;
			if (!maxPicWidth || (maxPicWidth < vps->rep_formats[format_idx].pic_width_luma_samples))
				maxPicWidth = vps->rep_formats[format_idx].pic_width_luma_samples;
			if (!maxPicHeight || (maxPicHeight < vps->rep_formats[format_idx].pic_height_luma_samples))
				maxPicHeight = vps->rep_formats[format_idx].pic_height_luma_samples;
			if (!maxChromaFormat || (maxChromaFormat < vps->rep_formats[format_idx].chroma_format_idc))
				maxChromaFormat = vps->rep_formats[format_idx].chroma_format_idc;
			bitDepth = vps->rep_formats[format_idx].bit_depth_chroma > vps->rep_formats[format_idx].bit_depth_luma ? vps->rep_formats[format_idx].bit_depth_chroma : vps->rep_formats[format_idx].bit_depth_luma;
			if (!maxChromaFormat || (maxChromaFormat < bitDepth))
				maxChromaFormat = bitDepth;
		}
		op->max_temporal_id = maxTemporalId;
		op->minPicWidth = minPicWidth;
		op->minPicHeight = minPicHeight;
		op->maxPicWidth = maxPicWidth;
		op->maxPicHeight = maxPicHeight;
		op->maxChromaFormat = maxChromaFormat;
		op->maxBitDepth = maxBitDepth;
		op->frame_rate_info_flag = GF_FALSE; //FIXME: should fetch this info from VUI
		op->bit_rate_info_flag = GF_FALSE; //we don't use it
		gf_list_add(oinf->operating_points, op);
	}

	for (i = 0; i < vps->max_layers; i++) {
		LHEVC_DependentLayer *dep;
		u32 j, k;
		GF_SAFEALLOC(dep, LHEVC_DependentLayer);
		dep->dependent_layerID = vps->layer_id_in_nuh[i];
		for (j = 0; j < vps->max_layers; j++) {
			if (vps->direct_dependency_flag[dep->dependent_layerID][j]) {
				dep->dependent_on_layerID[dep->num_layers_dependent_on] = j;
				dep->num_layers_dependent_on ++;
			}
		}
		k = 0;
		for (j = 0; j < 16; j++) {
			if (oinf->scalability_mask & (1 << j)) {
				dep->dimension_identifier[j] = vps->dimension_id[i][k];
				k++;
			}
		}
		gf_list_add(oinf->dependency_layers, dep);
	}

	//write Operating Points Information Sample Group
	bs = gf_bs_new(NULL, 0, GF_BITSTREAM_WRITE);
	gf_isom_oinf_write_entry(oinf, bs);
	gf_bs_get_content(bs, &data, &data_size);
	gf_bs_del(bs);
	gf_isom_oinf_del_entry(oinf);
	gf_isom_add_sample_group_info(file, hevc_track ? hevc_track : track, GF_ISOM_SAMPLE_GROUP_OINF, data, data_size, GF_TRUE, &di);

	if (hevc_track) {
		gf_isom_set_track_reference(file, track, GF_ISOM_REF_OREF, gf_isom_get_track_id(file, hevc_track) );
	}
	gf_free(data);
	return GF_OK;
}


typedef struct
{
	u32 layer_id_plus_one;
	u32 min_temporal_id, max_temporal_id;
} LHVCLayerInfo;

static void gf_lhevc_set_layer_information(GF_ISOFile *file, u32 track, LHVCLayerInfo *linf)
{
	u32 i, nb_layers=0, di=0;
	char *data;
	u32 data_size;

	GF_BitStream *bs = gf_bs_new(NULL, 0, GF_BITSTREAM_WRITE);

	for (i=0; i<64; i++) {
		if (linf[i].layer_id_plus_one) nb_layers++;
	}
	gf_bs_write_int(bs, 0, 2);
	gf_bs_write_int(bs, nb_layers, 6);
	for (i=0; i<nb_layers; i++) {
		if (! linf[i].layer_id_plus_one) continue;
		gf_bs_write_int(bs, 0, 4);
		gf_bs_write_int(bs, linf[i].layer_id_plus_one - 1, 6);
		gf_bs_write_int(bs, linf[i].min_temporal_id, 3);
		gf_bs_write_int(bs, linf[i].max_temporal_id, 3);
		gf_bs_write_int(bs, 0, 1);
		gf_bs_write_int(bs, 0xFF, 7);

	}
	gf_bs_get_content(bs, &data, &data_size);
	gf_bs_del(bs);
	gf_isom_add_sample_group_info(file, track, GF_ISOM_SAMPLE_GROUP_LINF, data, data_size, GF_TRUE, &di);
	gf_free(data);
}

static GF_Err gf_import_hevc(GF_MediaImporter *import)
{
#ifdef GPAC_DISABLE_HEVC
	return GF_NOT_SUPPORTED;
#else
	Bool detect_fps;
	u64 nal_start, nal_end, total_size;
	u32 i, nal_size, track, trackID, di, cur_samp, nb_i, nb_idr, nb_p, nb_b, nb_sp, nb_si, nb_sei, max_w, max_h, max_w_b, max_h_b, max_total_delay, nb_nalus, hevc_base_track;
	s32 idx, sei_recovery_frame_count;
	u64 duration;
	GF_Err e;
	FILE *mdia;
	HEVCState hevc;
	GF_AVCConfigSlot *slc;
	GF_HEVCConfig *hevc_cfg, *lhvc_cfg, *dst_cfg;
	GF_HEVCParamArray *spss, *ppss, *vpss;
	GF_BitStream *bs;
	GF_BitStream *sample_data;
	Bool flush_sample, flush_next_sample, is_empty_sample, sample_has_islice, sample_has_vps, sample_has_sps, is_islice, first_nal, slice_is_ref, has_cts_offset, is_paff, set_subsamples, slice_force_ref;
	u32 ref_frame, timescale, copy_size, size_length, dts_inc;
	s32 last_poc, max_last_poc, max_last_b_poc, poc_diff, prev_last_poc, min_poc, poc_shift;
	Bool first_hevc, has_hevc, has_lhvc;
	u32 use_opengop_gdr = 0;
	u8 layer_ids[64];
	SAPType sample_rap_type;
	s32 cur_vps_id = -1;
	u8 max_temporal_id[64];
	u32 min_layer_id = (u32) -1;
	LHVCLayerInfo linf[64];


	Double FPS;
	char *buffer;
	u32 max_size = 4096;

	if (import->flags & GF_IMPORT_PROBE_ONLY) {
		import->nb_tracks = 1;
		import->tk_info[0].track_num = 1;
		import->tk_info[0].stream_type = GF_STREAM_VISUAL;
		import->tk_info[0].flags = GF_IMPORT_OVERRIDE_FPS | GF_IMPORT_FORCE_PACKED;
		return GF_OK;
	}

	memset(linf, 0, sizeof(linf));

	set_subsamples = (import->flags & GF_IMPORT_SET_SUBSAMPLES) ? GF_TRUE : GF_FALSE;

	mdia = gf_fopen(import->in_name, "rb");
	if (!mdia) return gf_import_message(import, GF_URL_ERROR, "Cannot find file %s", import->in_name);

	detect_fps = GF_TRUE;
	FPS = (Double) import->video_fps;
	if (!FPS) {
		FPS = GF_IMPORT_DEFAULT_FPS;
	} else {
		if (import->video_fps == GF_IMPORT_AUTO_FPS) {
			import->video_fps = GF_IMPORT_DEFAULT_FPS;	/*fps=auto is handled as auto-detection is h264*/
		} else {
			/*fps is forced by the caller*/
			detect_fps = GF_FALSE;
		}
	}
	get_video_timing(FPS, &timescale, &dts_inc);

	poc_diff = 0;

restart_import:

	memset(&hevc, 0, sizeof(HEVCState));
	hevc.sps_active_idx = -1;
	dst_cfg = hevc_cfg = gf_odf_hevc_cfg_new();
	lhvc_cfg = gf_odf_hevc_cfg_new();
	lhvc_cfg->complete_representation = GF_TRUE;
	lhvc_cfg->is_lhvc = GF_TRUE;
	buffer = (char*)gf_malloc(sizeof(char) * max_size);
	sample_data = NULL;
	first_hevc = GF_TRUE;
	sei_recovery_frame_count = -1;
	spss = ppss = vpss = NULL;
	nb_nalus = 0;
	hevc_base_track = 0;
	has_hevc = has_lhvc = GF_FALSE;

	bs = gf_bs_from_file(mdia, GF_BITSTREAM_READ);
	if (!gf_media_nalu_is_start_code(bs)) {
		e = gf_import_message(import, GF_NON_COMPLIANT_BITSTREAM, "Cannot find HEVC start code");
		goto exit;
	}

	/*NALU size packing disabled*/
	if (!(import->flags & GF_IMPORT_FORCE_PACKED)) size_length = 32;
	/*if import in edit mode, use smallest NAL size and adjust on the fly*/
	else if (gf_isom_get_mode(import->dest)!=GF_ISOM_OPEN_WRITE) size_length = 8;
	else size_length = 32;

	trackID = 0;

	if (import->esd) trackID = import->esd->ESID;

	track = gf_isom_new_track(import->dest, trackID, GF_ISOM_MEDIA_VISUAL, timescale);
	if (!track) {
		e = gf_isom_last_error(import->dest);
		goto exit;
	}
	gf_isom_set_track_enabled(import->dest, track, 1);
	if (import->esd && !import->esd->ESID) import->esd->ESID = gf_isom_get_track_id(import->dest, track);
	import->final_trackID = gf_isom_get_track_id(import->dest, track);
	if (import->esd && import->esd->dependsOnESID) {
		gf_isom_set_track_reference(import->dest, track, GF_ISOM_REF_DECODE, import->esd->dependsOnESID);
	}

	e = gf_isom_hevc_config_new(import->dest, track, hevc_cfg, NULL, NULL, &di);
	if (e) goto exit;

	gf_isom_set_nalu_extract_mode(import->dest, track, GF_ISOM_NALU_EXTRACT_INSPECT);
	memset(layer_ids, 0, sizeof(u8)*64);

	sample_data = NULL;
	sample_rap_type = RAP_NO;
	sample_has_islice = GF_FALSE;
	sample_has_sps = GF_FALSE;
	sample_has_vps = GF_FALSE;
	cur_samp = 0;
	is_paff = GF_FALSE;
	total_size = gf_bs_get_size(bs);
	nal_start = gf_bs_get_position(bs);
	duration = (u64) ( ((Double)import->duration) * timescale / 1000.0);

	nb_i = nb_idr = nb_p = nb_b = nb_sp = nb_si = nb_sei = 0;
	max_w = max_h = max_w_b = max_h_b = 0;
	first_nal = GF_TRUE;
	ref_frame = 0;
	last_poc = max_last_poc = max_last_b_poc = prev_last_poc = 0;
	max_total_delay = 0;

	gf_isom_set_cts_packing(import->dest, track, GF_TRUE);
	has_cts_offset = GF_FALSE;
	min_poc = 0;
	poc_shift = 0;
	flush_next_sample = GF_FALSE;
	is_empty_sample = GF_TRUE;
	memset(max_temporal_id, 0, 64*sizeof(u8));

	while (gf_bs_available(bs)) {
		s32 res;
		GF_HEVCConfig *prev_cfg;
		u8 nal_unit_type, temporal_id, layer_id;
		Bool skip_nal, add_sps, is_slice, has_vcl_nal;
		u32 nal_and_trailing_size;

		has_vcl_nal = GF_FALSE;
		nal_and_trailing_size = nal_size = gf_media_nalu_next_start_code_bs(bs);
		if (!(import->flags & GF_IMPORT_KEEP_TRAILING)) {
			nal_size = gf_media_nalu_payload_end_bs(bs);
		}


		if (nal_size>max_size) {
			buffer = (char*)gf_realloc(buffer, sizeof(char)*nal_size);
			max_size = nal_size;
		}

		/*read the file, and work on a memory buffer*/
		gf_bs_read_data(bs, buffer, nal_size);

//		gf_bs_seek(bs, nal_start);

		res = gf_media_hevc_parse_nalu(buffer, nal_size, &hevc, &nal_unit_type, &temporal_id, &layer_id);

		if (max_temporal_id[layer_id] < temporal_id)
			max_temporal_id[layer_id] = temporal_id;

		if (layer_id && (import->flags & GF_IMPORT_SVC_NONE)) {
			goto next_nal;
		}

		nb_nalus++;

		is_islice = GF_FALSE;

		prev_cfg = dst_cfg;

		if (layer_id) {
			dst_cfg = lhvc_cfg;
			has_lhvc = GF_TRUE;
		} else {
			dst_cfg = hevc_cfg;
			has_hevc = GF_TRUE;
		}

		if (prev_cfg != dst_cfg) {
			vpss = get_hevc_param_array(dst_cfg, GF_HEVC_NALU_VID_PARAM);
			spss = get_hevc_param_array(dst_cfg, GF_HEVC_NALU_SEQ_PARAM);
			ppss = get_hevc_param_array(dst_cfg, GF_HEVC_NALU_PIC_PARAM);
		}

		skip_nal = GF_FALSE;
		copy_size = flush_sample = GF_FALSE;
		is_slice = GF_FALSE;

		switch (res) {
		case 1:
			if (import->flags & GF_IMPORT_FORCE_XPS_INBAND) {
				if (!is_empty_sample)
					flush_sample = GF_TRUE;
			} else {
				flush_sample = GF_TRUE;
			}
			break;
		case -1:
			gf_import_message(import, GF_OK, "Waring: Error parsing NAL unit");
			skip_nal = GF_TRUE;
			break;
		case -2:
			skip_nal = GF_TRUE;
			break;
		default:
			break;
		}

		if ( (layer_id == min_layer_id) && flush_next_sample && (nal_unit_type!=GF_HEVC_NALU_SEI_SUFFIX)) {
			flush_next_sample = GF_FALSE;
			flush_sample = GF_TRUE;
		}

		switch (nal_unit_type) {
		case GF_HEVC_NALU_VID_PARAM:
			if (import->flags & GF_IMPORT_NO_VPS_EXTENSIONS) {
				//this may modify nal_size, but we don't use it for bitstream reading
				idx = gf_media_hevc_read_vps_ex(buffer, &nal_size, &hevc, GF_TRUE);
			} else {
				idx = hevc.last_parsed_vps_id;
			}
			if (idx<0) {
				e = gf_import_message(import, GF_NON_COMPLIANT_BITSTREAM, "Error parsing Video Param");
				goto exit;
			}
			/*if we get twice the same VPS put in the the bitstream and set array_completeness to 0 ...*/
			if (hevc.vps[idx].state == 2) {
				if (hevc.vps[idx].crc != gf_crc_32(buffer, nal_size)) {
					copy_size = nal_size;
					assert(vpss);
					vpss->array_completeness = 0;
				}
			}

			if (hevc.vps[idx].state==1) {
				hevc.vps[idx].state = 2;
				hevc.vps[idx].crc = gf_crc_32(buffer, nal_size);

				dst_cfg->avgFrameRate = hevc.vps[idx].rates[0].avg_pic_rate;
				dst_cfg->constantFrameRate = hevc.vps[idx].rates[0].constand_pic_rate_idc;
				dst_cfg->numTemporalLayers = hevc.vps[idx].max_sub_layers;
				dst_cfg->temporalIdNested = hevc.vps[idx].temporal_id_nesting;
				//TODO set scalability mask

				if (!vpss) {
					GF_SAFEALLOC(vpss, GF_HEVCParamArray);
					vpss->nalus = gf_list_new();
					gf_list_add(dst_cfg->param_array, vpss);
					vpss->array_completeness = 1;
					vpss->type = GF_HEVC_NALU_VID_PARAM;
				}

				if (import->flags & GF_IMPORT_FORCE_XPS_INBAND) {
					vpss->array_completeness = 0;
					copy_size = nal_size;
					sample_has_vps = GF_TRUE;
				}

				slc = (GF_AVCConfigSlot*)gf_malloc(sizeof(GF_AVCConfigSlot));
				slc->size = nal_size;
				slc->id = idx;
				slc->data = (char*)gf_malloc(sizeof(char)*slc->size);
				memcpy(slc->data, buffer, sizeof(char)*slc->size);

				gf_list_add(vpss->nalus, slc);

			}

			if (import->flags & GF_IMPORT_FORCE_XPS_INBAND) {
				copy_size = nal_size;
				if (!is_empty_sample)
					flush_sample = GF_TRUE;
			}

			cur_vps_id = idx;
			break;
		case GF_HEVC_NALU_SEQ_PARAM:
			idx = hevc.last_parsed_sps_id;
			if (idx<0) {
				e = gf_import_message(import, GF_NON_COMPLIANT_BITSTREAM, "Error parsing SeqInfo");
				break;
			}
			add_sps = GF_FALSE;
			if ((hevc.sps[idx].state & AVC_SPS_PARSED) && !(hevc.sps[idx].state & AVC_SPS_DECLARED)) {
				hevc.sps[idx].state |= AVC_SPS_DECLARED;
				add_sps = GF_TRUE;
				hevc.sps[idx].crc = gf_crc_32(buffer, nal_size);
			}

			/*if we get twice the same SPS put it in the bitstream and set array_completeness to 0 ...*/
			else if (hevc.sps[idx].state & AVC_SPS_DECLARED) {
				if (hevc.sps[idx].crc != gf_crc_32(buffer, nal_size)) {
					copy_size = nal_size;
					assert(spss);
					spss->array_completeness = 0;
				}
			}

			if (add_sps) {
				dst_cfg->configurationVersion = 1;
				dst_cfg->profile_space = hevc.sps[idx].ptl.profile_space;
				dst_cfg->tier_flag = hevc.sps[idx].ptl.tier_flag;
				dst_cfg->profile_idc = hevc.sps[idx].ptl.profile_idc;
				dst_cfg->general_profile_compatibility_flags = hevc.sps[idx].ptl.profile_compatibility_flag;
				dst_cfg->progressive_source_flag = hevc.sps[idx].ptl.general_progressive_source_flag;
				dst_cfg->interlaced_source_flag = hevc.sps[idx].ptl.general_interlaced_source_flag;
				dst_cfg->non_packed_constraint_flag = hevc.sps[idx].ptl.general_non_packed_constraint_flag;
				dst_cfg->frame_only_constraint_flag = hevc.sps[idx].ptl.general_frame_only_constraint_flag;

				dst_cfg->constraint_indicator_flags = hevc.sps[idx].ptl.general_reserved_44bits;
				dst_cfg->level_idc = hevc.sps[idx].ptl.level_idc;

				dst_cfg->chromaFormat = hevc.sps[idx].chroma_format_idc;
				dst_cfg->luma_bit_depth = hevc.sps[idx].bit_depth_luma;
				dst_cfg->chroma_bit_depth = hevc.sps[idx].bit_depth_chroma;

				if (!spss) {
					GF_SAFEALLOC(spss, GF_HEVCParamArray);
					spss->nalus = gf_list_new();
					gf_list_add(dst_cfg->param_array, spss);
					spss->array_completeness = 1;
					spss->type = GF_HEVC_NALU_SEQ_PARAM;
				}

				/*disable frame rate scan, most bitstreams have wrong values there*/
				if (detect_fps && hevc.sps[idx].has_timing_info
				        /*if detected FPS is greater than 1000, assume wrong timing info*/
				        && (hevc.sps[idx].time_scale <= 1000*hevc.sps[idx].num_units_in_tick)
				   ) {
					timescale = hevc.sps[idx].time_scale;
					dts_inc =   hevc.sps[idx].num_units_in_tick;
					FPS = (Double)timescale / dts_inc;
					detect_fps = GF_FALSE;
					gf_isom_remove_track(import->dest, track);
					if (sample_data) gf_bs_del(sample_data);
					gf_odf_hevc_cfg_del(hevc_cfg);
					hevc_cfg = NULL;
					gf_odf_hevc_cfg_del(lhvc_cfg);
					lhvc_cfg = NULL;
					gf_free(buffer);
					buffer = NULL;
					gf_bs_del(bs);
					bs = NULL;
					gf_fseek(mdia, 0, SEEK_SET);
					goto restart_import;
				}

				if (import->flags & GF_IMPORT_FORCE_XPS_INBAND) {
					spss->array_completeness = 0;
					copy_size = nal_size;
				}

				slc = (GF_AVCConfigSlot*)gf_malloc(sizeof(GF_AVCConfigSlot));
				slc->size = nal_size;
				slc->id = idx;
				slc->data = (char*)gf_malloc(sizeof(char)*slc->size);
				memcpy(slc->data, buffer, sizeof(char)*slc->size);
				gf_list_add(spss->nalus, slc);

				if (first_hevc) {
					first_hevc = GF_FALSE;
					gf_import_message(import, GF_OK, "HEVC import - frame size %d x %d at %02.3f FPS", hevc.sps[idx].width, hevc.sps[idx].height, FPS);
				} else {
					gf_import_message(import, GF_OK, "LHVC detected - %d x %d at %02.3f FPS", hevc.sps[idx].width, hevc.sps[idx].height, FPS);
				}

				if ((max_w <= hevc.sps[idx].width) && (max_h <= hevc.sps[idx].height)) {
					max_w = hevc.sps[idx].width;
					max_h = hevc.sps[idx].height;
				}
				if (!layer_id && (max_w_b <= hevc.sps[idx].width) && (max_h_b <= hevc.sps[idx].height)) {
					max_w_b = hevc.sps[idx].width;
					max_h_b = hevc.sps[idx].height;
				}
			}
			if (import->flags & GF_IMPORT_FORCE_XPS_INBAND) {
				copy_size = nal_size;
				sample_has_sps = GF_TRUE;
				if (!is_empty_sample)
					flush_sample = GF_TRUE;
			}
			break;

		case GF_HEVC_NALU_PIC_PARAM:
			idx = hevc.last_parsed_pps_id;
			if (idx<0) {
				e = gf_import_message(import, GF_NON_COMPLIANT_BITSTREAM, "Error parsing Picture Param");
				goto exit;
			}
			/*if we get twice the same PPS put it in the bitstream and set array_completeness to 0 ...*/
			if (hevc.pps[idx].state == 2) {
				if (hevc.pps[idx].crc != gf_crc_32(buffer, nal_size)) {
					copy_size = nal_size;
					assert(ppss);
					ppss->array_completeness = 0;
				}
			}

			if (hevc.pps[idx].state==1) {
				hevc.pps[idx].state = 2;
				hevc.pps[idx].crc = gf_crc_32(buffer, nal_size);

				if (!ppss) {
					GF_SAFEALLOC(ppss, GF_HEVCParamArray);
					ppss->nalus = gf_list_new();
					gf_list_add(dst_cfg->param_array, ppss);
					ppss->array_completeness = 1;
					ppss->type = GF_HEVC_NALU_PIC_PARAM;
				}

				if (import->flags & GF_IMPORT_FORCE_XPS_INBAND) {
					ppss->array_completeness = 0;
					copy_size = nal_size;
				}

				slc = (GF_AVCConfigSlot*)gf_malloc(sizeof(GF_AVCConfigSlot));
				slc->size = nal_size;
				slc->id = idx;
				slc->data = (char*)gf_malloc(sizeof(char)*slc->size);
				memcpy(slc->data, buffer, sizeof(char)*slc->size);

				gf_list_add(ppss->nalus, slc);
			}
			if (import->flags & GF_IMPORT_FORCE_XPS_INBAND) {
				copy_size = nal_size;
				if (!is_empty_sample)
					flush_sample = GF_TRUE;
			}

			break;
		case GF_HEVC_NALU_SEI_SUFFIX:
			if (import->flags & GF_IMPORT_NO_SEI) {
					copy_size = 0;
			} else {
				if (hevc.sps_active_idx != -1) {
					copy_size = nal_size;
					if (!layer_id) {
						if (!is_empty_sample) flush_next_sample = GF_TRUE;
						else copy_size = 0;
					}
					if (copy_size)
						nb_sei++;
				}
			}
			break;
		case GF_HEVC_NALU_SEI_PREFIX:
			if (import->flags & GF_IMPORT_NO_SEI) {
				copy_size = 0;
			} else {
				if (hevc.sps_active_idx != -1) {
					copy_size = nal_size;
					if (copy_size) {
						nb_sei++;
					}
				}
			}
			if (nal_size) {
				//FIXME should not be minus 1 in layer_ids[layer_id - 1] but the previous layer in the tree
				if (!layer_id || !layer_ids[layer_id - 1]) flush_sample = GF_TRUE;
			}
			break;

		/*slice_segment_layer_rbsp*/
		case GF_HEVC_NALU_SLICE_STSA_N:
		case GF_HEVC_NALU_SLICE_STSA_R:
		case GF_HEVC_NALU_SLICE_RADL_R:
		case GF_HEVC_NALU_SLICE_RASL_R:
		case GF_HEVC_NALU_SLICE_RADL_N:
		case GF_HEVC_NALU_SLICE_RASL_N:
		case GF_HEVC_NALU_SLICE_TRAIL_N:
		case GF_HEVC_NALU_SLICE_TRAIL_R:
		case GF_HEVC_NALU_SLICE_TSA_N:
		case GF_HEVC_NALU_SLICE_TSA_R:
		case GF_HEVC_NALU_SLICE_BLA_W_LP:
		case GF_HEVC_NALU_SLICE_BLA_W_DLP:
		case GF_HEVC_NALU_SLICE_BLA_N_LP:
		case GF_HEVC_NALU_SLICE_IDR_W_DLP:
		case GF_HEVC_NALU_SLICE_IDR_N_LP:
		case GF_HEVC_NALU_SLICE_CRA:
			is_slice = GF_TRUE;
			if (min_layer_id > layer_id)
				min_layer_id = layer_id;
			/*			if ((hevc.s_info.slice_segment_address<=100) || (hevc.s_info.slice_segment_address>=200))
							skip_nal = 1;
						if (!hevc.s_info.slice_segment_address)
							skip_nal = 0;
			*/
			if (! skip_nal) {
				copy_size = nal_size;
				has_vcl_nal = GF_TRUE;
				switch (hevc.s_info.slice_type) {
				case GF_HEVC_SLICE_TYPE_P:
					nb_p++;
					break;
				case GF_HEVC_SLICE_TYPE_I:
					nb_i++;
					is_islice = GF_TRUE;
					break;
				case GF_HEVC_SLICE_TYPE_B:
					nb_b++;
					break;
				}
			}
			break;

		/*remove*/
		case GF_HEVC_NALU_ACCESS_UNIT:
		case GF_HEVC_NALU_FILLER_DATA:
		case GF_HEVC_NALU_END_OF_SEQ:
		case GF_HEVC_NALU_END_OF_STREAM:
			break;

		default:
			gf_import_message(import, GF_OK, "WARNING: NAL Unit type %d not handled - adding", nal_unit_type);
			copy_size = nal_size;
			break;
		}

		if (!nal_size) break;
		if (copy_size) {
			linf[layer_id].layer_id_plus_one = layer_id + 1;
			if (! linf[layer_id].max_temporal_id ) linf[layer_id].max_temporal_id = temporal_id;
			else if (linf[layer_id].max_temporal_id < temporal_id) linf[layer_id].max_temporal_id = temporal_id;

			if (! linf[layer_id].min_temporal_id ) linf[layer_id].min_temporal_id = temporal_id;
			else if (linf[layer_id].min_temporal_id > temporal_id) linf[layer_id].min_temporal_id = temporal_id;
		}

		if (flush_sample && is_empty_sample)
			flush_sample = GF_FALSE;

		if (flush_sample && sample_data) {
			GF_ISOSample *samp = gf_isom_sample_new();
			samp->DTS = (u64)dts_inc*cur_samp;
			samp->IsRAP = ((sample_rap_type==SAP_TYPE_1) || (sample_rap_type==SAP_TYPE_2)) ? RAP : RAP_NO;
			if (! samp->IsRAP) {
				if (sample_has_islice && (import->flags & GF_IMPORT_FORCE_SYNC) && (sei_recovery_frame_count==0)) {
					samp->IsRAP = RAP;
					if (!use_opengop_gdr) {
						use_opengop_gdr = 1;
						GF_LOG(GF_LOG_WARNING, GF_LOG_CODING, ("[HEVC Import] Forcing non-IDR samples with I slices to be marked as sync points - resulting file will not be ISO conformant\n"));
					}
				}
			}
			gf_bs_get_content(sample_data, &samp->data, &samp->dataLength);
			gf_bs_del(sample_data);
			sample_data = NULL;

			//fixme, we should check sps and vps IDs when missing
			if ((import->flags & GF_IMPORT_FORCE_XPS_INBAND) && sample_rap_type && (!sample_has_vps || !sample_has_sps) ) {
				u32 k;
				GF_BitStream *fbs = gf_bs_new(NULL, 0, GF_BITSTREAM_WRITE);
				if (!sample_has_vps) {
					if (!vpss)
						vpss = get_hevc_param_array(hevc_cfg, GF_HEVC_NALU_VID_PARAM);
					assert(vpss);
					for (k=0;k<gf_list_count(vpss->nalus); k++) {
						GF_AVCConfigSlot *slc = gf_list_get(vpss->nalus, k);
						gf_bs_write_int(fbs, slc->size, size_length);
						gf_bs_write_data(fbs, slc->data, slc->size);
					}
				}
				if (!sample_has_sps) {
					if (!spss)
						spss = get_hevc_param_array(hevc_cfg, GF_HEVC_NALU_SEQ_PARAM);
					assert(spss);
					for (k=0;k<gf_list_count(spss->nalus); k++) {
						GF_AVCConfigSlot *slc = gf_list_get(spss->nalus, k);
						gf_bs_write_int(fbs, slc->size, size_length);
						gf_bs_write_data(fbs, slc->data, slc->size);
					}
				}
				gf_bs_write_data(fbs, samp->data, samp->dataLength);
				gf_bs_get_content(fbs, &samp->data, &samp->dataLength);
				gf_bs_del(fbs);
			}

			/*CTS recomuting is much trickier than with MPEG-4 ASP due to b-slice used as references - we therefore
			store the POC as the CTS offset and update the whole table at the end*/
			samp->CTS_Offset = last_poc - poc_shift;
			assert(last_poc >= poc_shift);
			e = gf_isom_add_sample(import->dest, track, di, samp);
			if (e) goto exit;

			cur_samp++;

			/*write sampleGroups info*/
			if (!samp->IsRAP && ((sei_recovery_frame_count>=0) || sample_has_islice || (sample_rap_type && (sample_rap_type<=SAP_TYPE_3)) ) ) {
				/*generic GDR*/
				if (sei_recovery_frame_count > 0) {
					if (!use_opengop_gdr) use_opengop_gdr = 1;
					e = gf_isom_set_sample_roll_group(import->dest, track, cur_samp, (s16) sei_recovery_frame_count);
				}
				/*open-GOP*/
				else if (sample_rap_type==SAP_TYPE_3) {
					if (!min_layer_id && !use_opengop_gdr) use_opengop_gdr = 2;
					e = gf_isom_set_sample_rap_group(import->dest, track, cur_samp, 0);
				}
				if (e) goto exit;
			}

			gf_isom_sample_del(&samp);
			gf_set_progress("Importing HEVC", (u32) (nal_start/1024), (u32) (total_size/1024) );
			first_nal = GF_TRUE;

			if (min_poc > last_poc)
				min_poc = last_poc;

			sample_has_islice = GF_FALSE;
			sample_has_vps = GF_FALSE;
			sample_has_sps = GF_FALSE;
			sei_recovery_frame_count = -1;
			is_empty_sample = GF_TRUE;
		}

		if (copy_size) {
			if (is_islice)
				sample_has_islice = GF_TRUE;

			if ((size_length<32) && ( (u32) (1<<size_length)-1 < copy_size)) {
				u32 diff_size = 8;
				while ((size_length<32) && ( (u32) (1<<(size_length+diff_size))-1 < copy_size)) diff_size+=8;
				/*only 8bits, 16bits and 32 bits*/
				if (size_length+diff_size == 24) diff_size+=8;

				gf_import_message(import, GF_OK, "Adjusting HEVC SizeLength to %d bits", size_length+diff_size);
				gf_media_avc_rewrite_samples(import->dest, track, size_length, size_length+diff_size);

				/*rewrite current sample*/
				if (sample_data) {
					char *sd;
					u32 sd_l;
					GF_BitStream *prev_sd;
					gf_bs_get_content(sample_data, &sd, &sd_l);
					gf_bs_del(sample_data);
					sample_data = gf_bs_new(NULL, 0, GF_BITSTREAM_WRITE);
					prev_sd = gf_bs_new(sd, sd_l, GF_BITSTREAM_READ);
					while (gf_bs_available(prev_sd)) {
						char *buf;
						u32 s = gf_bs_read_int(prev_sd, size_length);
						gf_bs_write_int(sample_data, s, size_length+diff_size);
						buf = (char*)gf_malloc(sizeof(char)*s);
						gf_bs_read_data(prev_sd, buf, s);
						gf_bs_write_data(sample_data, buf, s);
						gf_free(buf);
					}
					gf_bs_del(prev_sd);
					gf_free(sd);
				}
				size_length+=diff_size;

			}
			if (!sample_data) sample_data = gf_bs_new(NULL, 0, GF_BITSTREAM_WRITE);
			gf_bs_write_int(sample_data, copy_size, size_length);
			gf_bs_write_data(sample_data, buffer, copy_size);

			if (set_subsamples) {
				/* use the res and priority value of last prefix NALU */
				gf_isom_add_subsample(import->dest, track, cur_samp+1, 0, copy_size+size_length/8, 0, 0, GF_FALSE);
			}

			if (has_vcl_nal) {
				is_empty_sample = GF_FALSE;
			}
			layer_ids[layer_id] = 1;

			if ((layer_id == min_layer_id) && is_slice) {
				slice_is_ref = gf_media_hevc_slice_is_IDR(&hevc);
				if (slice_is_ref)
					nb_idr++;
				slice_force_ref = GF_FALSE;

				/*we only indicate TRUE IDRs for sync samples (cf AVC file format spec).
				SEI recovery should be used to build sampleToGroup & RollRecovery tables*/
				if (first_nal) {
					first_nal = GF_FALSE;
					if (hevc.sei.recovery_point.valid || (import->flags & GF_IMPORT_FORCE_SYNC)) {
						Bool bIntraSlice = gf_media_hevc_slice_is_intra(&hevc);
						sei_recovery_frame_count = hevc.sei.recovery_point.frame_cnt;

						/*we allow to mark I-frames as sync on open-GOPs (with sei_recovery_frame_count=0) when forcing sync even when the SEI RP is not available*/
						if (!hevc.sei.recovery_point.valid && bIntraSlice) {
							sei_recovery_frame_count = 0;
							if (use_opengop_gdr == 1) {
								use_opengop_gdr = 2; /*avoid message flooding*/
								GF_LOG(GF_LOG_WARNING, GF_LOG_CODING, ("[HEVC Import] No valid SEI Recovery Point found although needed - forcing\n"));
							}
						}
						hevc.sei.recovery_point.valid = 0;
						if (bIntraSlice && (import->flags & GF_IMPORT_FORCE_SYNC) && (sei_recovery_frame_count==0))
							slice_force_ref = GF_TRUE;
					}
					sample_rap_type = RAP_NO;
					if (gf_media_hevc_slice_is_IDR(&hevc)) {
						sample_rap_type = SAP_TYPE_1;
					}
					else {
						switch (hevc.s_info.nal_unit_type) {
						case GF_HEVC_NALU_SLICE_BLA_W_LP:
						case GF_HEVC_NALU_SLICE_BLA_W_DLP:
							sample_rap_type = SAP_TYPE_3;
							break;
						case GF_HEVC_NALU_SLICE_BLA_N_LP:
							sample_rap_type = SAP_TYPE_1;
							break;
						case GF_HEVC_NALU_SLICE_CRA:
							sample_rap_type = SAP_TYPE_3;
							break;
						}
					}
				}

				if (hevc.s_info.poc<poc_shift) {
					u32 j;
					if (ref_frame) {
						for (j=ref_frame; j<=cur_samp; j++) {
							GF_ISOSample *samp = gf_isom_get_sample_info(import->dest, track, j, NULL, NULL);
							if (!samp) break;
							samp->CTS_Offset += poc_shift;
							samp->CTS_Offset -= hevc.s_info.poc;
							gf_isom_modify_cts_offset(import->dest, track, j, samp->CTS_Offset);
							gf_isom_sample_del(&samp);
						}
					}
					poc_shift = hevc.s_info.poc;
				}

				/*if #pics, compute smallest POC increase*/
				if (hevc.s_info.poc != last_poc) {
					if (!poc_diff || (poc_diff > abs(hevc.s_info.poc-last_poc))) {
						poc_diff = abs(hevc.s_info.poc - last_poc);/*ideally we would need to start the parsing again as poc_diff helps computing max_total_delay*/
					}
					last_poc = hevc.s_info.poc;
					assert(is_slice);
				}

				/*ref slice, reset poc*/
				if (slice_is_ref) {
					ref_frame = cur_samp+1;
					max_last_poc = last_poc = max_last_b_poc = 0;
					poc_shift = 0;
				}
				/*forced ref slice*/
				else if (slice_force_ref) {
					ref_frame = cur_samp+1;
					/*adjust POC shift as sample will now be marked as sync, so wo must store poc as if IDR (eg POC=0) for our CTS offset computing to be correct*/
					poc_shift = hevc.s_info.poc;
				}
				/*strictly less - this is a new P slice*/
				else if (max_last_poc<last_poc) {
					max_last_b_poc = 0;
					//prev_last_poc = max_last_poc;
					max_last_poc = last_poc;
				}
				/*stricly greater*/
				else if (max_last_poc>last_poc) {
					/*need to store TS offsets*/
					has_cts_offset = GF_TRUE;
					switch (hevc.s_info.slice_type) {
					case GF_AVC_TYPE_B:
					case GF_AVC_TYPE2_B:
						if (!max_last_b_poc) {
							max_last_b_poc = last_poc;
						}
						/*if same poc than last max, this is a B-slice*/
						else if (last_poc>max_last_b_poc) {
							max_last_b_poc = last_poc;
						}
						/*otherwise we had a B-slice reference: do nothing*/

						break;
					}
				}

				/*compute max delay (applicable when B slice are present)*/
				if (ref_frame && poc_diff && (s32)(cur_samp-(ref_frame-1)-last_poc/poc_diff)>(s32)max_total_delay) {
					max_total_delay = cur_samp - (ref_frame-1) - last_poc/poc_diff;
				}
			}
		}

next_nal:
		gf_bs_align(bs);
		nal_end = gf_bs_get_position(bs);
		assert(nal_start <= nal_end);
		assert(nal_end <= nal_start + nal_and_trailing_size);
		if (nal_end != nal_start + nal_and_trailing_size)
			gf_bs_seek(bs, nal_start + nal_and_trailing_size);

		if (!gf_bs_available(bs)) break;
		if (duration && (dts_inc*cur_samp > duration)) break;
		if (import->flags & GF_IMPORT_DO_ABORT) break;

		/*consume next start code*/
		nal_start = gf_media_nalu_next_start_code_bs(bs);
		if (nal_start) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODING, ("[hevc] invalid nal_size (%u)? Skipping "LLU" bytes to reach next start code\n", nal_size, nal_start));
			gf_bs_skip_bytes(bs, nal_start);
		}
		nal_start = gf_media_nalu_is_start_code(bs);
		if (!nal_start) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODING, ("[hevc] error: no start code found ("LLU" bytes read out of "LLU") - leaving\n", gf_bs_get_position(bs), gf_bs_get_size(bs)));
			break;
		}
		nal_start = gf_bs_get_position(bs);
	}

	/*final flush*/
	if (sample_data) {
		GF_ISOSample *samp = gf_isom_sample_new();
		samp->DTS = (u64)dts_inc*cur_samp;
		samp->IsRAP = (sample_rap_type == SAP_TYPE_1) ? RAP : RAP_NO;
		if (!sample_rap_type && sample_has_islice && (import->flags & GF_IMPORT_FORCE_SYNC)) {
			samp->IsRAP = RAP;
		}
		/*we store the frame order (based on the POC) as the CTS offset and update the whole table at the end*/
		samp->CTS_Offset = last_poc - poc_shift;
		gf_bs_get_content(sample_data, &samp->data, &samp->dataLength);
		gf_bs_del(sample_data);
		sample_data = NULL;
		e = gf_isom_add_sample(import->dest, track, di, samp);
		if (e) goto exit;

		gf_isom_sample_del(&samp);
		gf_set_progress("Importing HEVC", (u32) cur_samp, cur_samp+1);
		cur_samp++;
	}


	/*recompute all CTS offsets*/
	if (has_cts_offset) {
		u32 last_cts_samp;
		u64 last_dts, max_cts, min_cts;
		if (!poc_diff) poc_diff = 1;
		/*no b-frame references, no need to cope with negative poc*/
		if (!max_total_delay) {
			min_poc=0;
			max_total_delay = 1;
		}
		cur_samp = gf_isom_get_sample_count(import->dest, track);
		min_poc *= -1;
		last_dts = 0;
		max_cts = 0;
		min_cts = (u64) -1;
		last_cts_samp = 0;

		for (i=0; i<cur_samp; i++) {
			u64 cts;
			/*not using descIdx and data_offset will only fecth DTS, CTS and RAP which is all we need*/
			GF_ISOSample *samp = gf_isom_get_sample_info(import->dest, track, i+1, NULL, NULL);
			/*poc re-init (RAP and POC to 0, otherwise that's SEI recovery), update base DTS*/
			if (samp->IsRAP /*&& !samp->CTS_Offset*/)
				last_dts = samp->DTS * (1+is_paff);

			/*CTS offset is frame POC (refers to last IDR)*/
			cts = (min_poc + (s32) samp->CTS_Offset) * dts_inc/poc_diff + (u32) last_dts;

			/*if PAFF, 2 pictures (eg poc) <=> 1 aggregated frame (eg sample), divide by 2*/
			if (is_paff) {
				cts /= 2;
				/*in some cases the poc is not on the top field - if that is the case, round up*/
				if (cts%dts_inc) {
					cts = ((cts/dts_inc)+1)*dts_inc;
				}
			}

			/*B-frames offset*/
			cts += (u32) (max_total_delay*dts_inc);

			samp->CTS_Offset = (u32) (cts - samp->DTS);

			if (max_cts < samp->DTS + samp->CTS_Offset) {
				max_cts = samp->DTS + samp->CTS_Offset;
				last_cts_samp = i;
			}
			if (min_cts > samp->DTS + samp->CTS_Offset) {
				min_cts = samp->DTS + samp->CTS_Offset;
			}

			/*this should never happen, however some streams seem to do weird POC increases (cf sorenson streams, last 2 frames),
			this should hopefully take care of some bugs and ensure proper CTS...*/
			if ((s32)samp->CTS_Offset<0) {
				u32 j, k;
				samp->CTS_Offset = 0;
				gf_isom_modify_cts_offset(import->dest, track, i+1, samp->CTS_Offset);
				for (j=last_cts_samp; j<i; j++) {
					GF_ISOSample *asamp = gf_isom_get_sample_info(import->dest, track, j+1, NULL, NULL);
					for (k=j+1; k<=i; k++) {
						GF_ISOSample *bsamp = gf_isom_get_sample_info(import->dest, track, k+1, NULL, NULL);
						if (asamp->CTS_Offset+asamp->DTS==bsamp->CTS_Offset+bsamp->DTS) {
							max_cts += dts_inc;
							bsamp->CTS_Offset = (u32) (max_cts - bsamp->DTS);
							gf_isom_modify_cts_offset(import->dest, track, k+1, bsamp->CTS_Offset);
						}
						gf_isom_sample_del(&bsamp);
					}
					gf_isom_sample_del(&asamp);
				}
				max_cts = samp->DTS + samp->CTS_Offset;
			} else {
				gf_isom_modify_cts_offset(import->dest, track, i+1, samp->CTS_Offset);
			}
			gf_isom_sample_del(&samp);
		}
		/*and repack table*/
		gf_isom_set_cts_packing(import->dest, track, GF_FALSE);

		if (!(import->flags & GF_IMPORT_NO_EDIT_LIST) && min_cts) {
			last_dts = max_cts - min_cts + gf_isom_get_sample_duration(import->dest, track, gf_isom_get_sample_count(import->dest, track) );
			last_dts *= gf_isom_get_timescale(import->dest);
			last_dts /= gf_isom_get_media_timescale(import->dest, track);
			gf_isom_set_edit_segment(import->dest, track, 0, last_dts, min_cts, GF_ISOM_EDIT_NORMAL);
		}
	} else {
		gf_isom_remove_cts_info(import->dest, track);
	}

	gf_set_progress("Importing HEVC", (u32) cur_samp, cur_samp);

	hevc_cfg->nal_unit_size = lhvc_cfg->nal_unit_size = size_length/8;


	//LHVC bitstream with external base layer
	if (min_layer_id != 0) {
		gf_isom_set_visual_info(import->dest, track, di, max_w, max_h);
		//Because layer_id of vps is 0, we need to clone vps from hevc_cfg to lhvc_cfg first
		for (i = 0; i < gf_list_count(hevc_cfg->param_array); i++) {
			u32 j, k, count2;
			GF_HEVCParamArray *s_ar = NULL;
			GF_HEVCParamArray *ar = gf_list_get(hevc_cfg->param_array, i);
			if (ar->type != GF_HEVC_NALU_VID_PARAM) continue;
			count2 = gf_list_count(ar->nalus);
			for (j=0; j<count2; j++) {
				GF_AVCConfigSlot *sl = gf_list_get(ar->nalus, j);
				GF_AVCConfigSlot *sl2;
				u8 layer_id = ((sl->data[0] & 0x1) << 5) | (sl->data[1] >> 3);
				if (layer_id) continue;

				for (k=0; k < gf_list_count(lhvc_cfg->param_array); k++) {
					s_ar = gf_list_get(lhvc_cfg->param_array, k);
					if (s_ar->type==GF_HEVC_NALU_VID_PARAM)
						break;
					s_ar = NULL;
				}
				if (!s_ar) {
					GF_SAFEALLOC(s_ar, GF_HEVCParamArray);
					s_ar->nalus = gf_list_new();
					s_ar->type = GF_HEVC_NALU_VID_PARAM;
					gf_list_insert(lhvc_cfg->param_array, s_ar, 0);
				}
				s_ar->array_completeness = ar->array_completeness;

				GF_SAFEALLOC(sl2, GF_AVCConfigSlot);
				sl2->data = gf_malloc(sl->size);
				memcpy(sl2->data, sl->data, sl->size);
				sl2->id = sl->id;
				sl2->size = sl->size;
				gf_list_add(s_ar->nalus, sl2);
			}
		}
		hevc_set_parall_type(lhvc_cfg);
		//must use LHV1/LHC1 since no base HEVC in the track
		gf_isom_lhvc_config_update(import->dest, track, 1, lhvc_cfg, GF_ISOM_LEHVC_ONLY);
	}
	//HEVC with optionnal lhvc
	else {
		gf_isom_set_visual_info(import->dest, track, di, max_w_b, max_h_b);
		hevc_set_parall_type(hevc_cfg);
		gf_isom_hevc_config_update(import->dest, track, 1, hevc_cfg);

		if (has_lhvc) {
			hevc_set_parall_type(lhvc_cfg);

			lhvc_cfg->avgFrameRate = hevc_cfg->avgFrameRate;
			lhvc_cfg->constantFrameRate = hevc_cfg->constantFrameRate;
			lhvc_cfg->numTemporalLayers = hevc_cfg->numTemporalLayers;
			lhvc_cfg->temporalIdNested = hevc_cfg->temporalIdNested;

			if (import->flags&GF_IMPORT_SVC_EXPLICIT) {
				gf_isom_lhvc_config_update(import->dest, track, 1, lhvc_cfg, GF_ISOM_LEHVC_WITH_BASE);
				gf_isom_modify_alternate_brand(import->dest, GF_ISOM_BRAND_HVCE, 1);
			} else {
				gf_isom_lhvc_config_update(import->dest, track, 1, lhvc_cfg, GF_ISOM_LEHVC_WITH_BASE_BACKWARD);
			}
		}
	}

	if (import->flags & GF_IMPORT_FORCE_XPS_INBAND) {
		gf_isom_hevc_set_inband_config(import->dest, track, 1);
	}

	gf_media_update_par(import->dest, track);
	gf_media_update_bitrate(import->dest, track);

	gf_isom_set_brand_info(import->dest, GF_ISOM_BRAND_ISO4, 1);
	gf_isom_modify_alternate_brand(import->dest, GF_ISOM_BRAND_ISOM, 0);
	if (has_hevc)
		gf_isom_modify_alternate_brand(import->dest, GF_ISOM_BRAND_HVC1, 1);

	if (!vpss && !ppss && !spss) {
		e = gf_import_message(import, GF_NON_COMPLIANT_BITSTREAM, "Import results: No SPS or PPS found in the bitstream ! Nothing imported\n");
	} else {
		if (nb_sp || nb_si) {
			gf_import_message(import, GF_OK, "HEVC Import results: %d samples (%d NALUs) - Slices: %d I %d P %d B %d SP %d SI - %d SEI - %d IDR",
			                  cur_samp, nb_nalus, nb_i, nb_p, nb_b, nb_sp, nb_si, nb_sei, nb_idr);
		} else {
			gf_import_message(import, GF_OK, "HEVC Import results: %d samples (%d NALUs) - Slices: %d I %d P %d B - %d SEI - %d IDR",
			                  cur_samp, nb_nalus, nb_i, nb_p, nb_b, nb_sei, nb_idr);
		}

		if (max_total_delay>1) {
			gf_import_message(import, GF_OK, "Stream uses forward prediction - stream CTS offset: %d frames", max_total_delay);
		}
	}

	if (use_opengop_gdr==2) {
		gf_import_message(import, GF_OK, "OpenGOP detected - adjusting file brand");
		gf_isom_modify_alternate_brand(import->dest, GF_ISOM_BRAND_ISO6, 1);
	}

	/*rewrite ESD*/
	if (import->esd) {
		if (!import->esd->slConfig) import->esd->slConfig = (GF_SLConfig*) gf_odf_desc_new(GF_ODF_SLC_TAG);
		import->esd->slConfig->predefined = 2;
		import->esd->slConfig->timestampResolution = timescale;
		if (import->esd->decoderConfig) gf_odf_desc_del((GF_Descriptor *)import->esd->decoderConfig);
		import->esd->decoderConfig = gf_isom_get_decoder_config(import->dest, track, 1);
		gf_isom_change_mpeg4_description(import->dest, track, 1, import->esd);
	}

	//base layer (i.e layer with layer_id = 0) not found in bitstream
	//we are importing an LHVC bitstream with external base layer
	//find this base layer with the imported tracks.
	//if we find more than one HEVC/AVC track, return an warning
	if (min_layer_id != 0) {
		u32 avc_base_track, ref_track_id;
		avc_base_track = hevc_base_track = 0;
		for (i = 1; i <= gf_isom_get_track_count(import->dest); i++) {
			u32 subtype = gf_isom_get_media_subtype(import->dest, i, 1);
			switch (subtype) {
			case GF_ISOM_SUBTYPE_AVC_H264:
			case GF_ISOM_SUBTYPE_AVC2_H264:
			case GF_ISOM_SUBTYPE_AVC3_H264:
			case GF_ISOM_SUBTYPE_AVC4_H264:
				if (!avc_base_track) {
					avc_base_track = i;
				} else {
					gf_import_message(import, GF_BAD_PARAM, "Warning: More than one AVC bitstream found, use track %d as base layer", avc_base_track);
				}
				break;
			case GF_ISOM_SUBTYPE_HVC1:
			case GF_ISOM_SUBTYPE_HEV1:
			case GF_ISOM_SUBTYPE_HVC2:
			case GF_ISOM_SUBTYPE_HEV2:
				if (!hevc_base_track) {
					hevc_base_track = i;
					if (avc_base_track) {
						gf_import_message(import, GF_BAD_PARAM, "Warning: Found both AVC and HEVC tracks, using HEVC track %d as base layer", hevc_base_track);
					}
				} else {
					gf_import_message(import, GF_BAD_PARAM, "Warning: More than one HEVC bitstream found, use track %d as base layer", avc_base_track);
				}
				break;
			}
		}
		if (!hevc_base_track && !avc_base_track) {
			gf_import_message(import, GF_BAD_PARAM, "Using LHVC external base layer, but no base layer not found - NOT SETTING SBAS TRACK REFERENCE!");;
		} else {
			ref_track_id = gf_isom_get_track_id(import->dest, hevc_base_track ? hevc_base_track : avc_base_track);
			gf_isom_set_track_reference(import->dest, track, GF_ISOM_REF_BASE, ref_track_id);
		}
	}

	// This is a L-HEVC bitstream ...
	if ( (has_lhvc && (cur_vps_id >= 0) && (cur_vps_id < 16) && (hevc.vps[cur_vps_id].max_layers > 1))
	// HEVC with several sublayers
	|| (max_temporal_id[0] > 0)
	) {
		gf_lhevc_set_operating_points_information(import->dest, hevc_base_track, track, &hevc.vps[cur_vps_id], max_temporal_id);
		gf_lhevc_set_layer_information(import->dest, track, &linf[0]);

		//sets track in group of type group_type and id track_group_id. If do_add is GF_FALSE, track is removed from that group
		e = gf_isom_set_track_group(import->dest, track, 1000+gf_isom_get_track_id(import->dest, track), GF_ISOM_BOX_TYPE_CSTG, GF_TRUE);

	}

exit:
	if (sample_data) gf_bs_del(sample_data);
	gf_odf_hevc_cfg_del(hevc_cfg);
	gf_odf_hevc_cfg_del(lhvc_cfg);
	gf_free(buffer);
	gf_bs_del(bs);
	gf_fclose(mdia);
	return e;
#endif //GPAC_DISABLE_HEVC
}

#endif /*GPAC_DISABLE_AV_PARSERS*/

static GF_Err gf_import_raw_unit(GF_MediaImporter *import)
{
	GF_Err e;
	GF_ISOSample *samp;
	u32 mtype, track, di, timescale, read;
	FILE *src;

	if (import->flags & GF_IMPORT_PROBE_ONLY) {
		import->flags |= GF_IMPORT_USE_DATAREF;
		return GF_OK;
	}

	if (!import->esd || !import->esd->decoderConfig) {
		return gf_import_message(import, GF_BAD_PARAM, "Raw stream needs ESD and DecoderConfig for import");
	}

	src = gf_fopen(import->in_name, "rb");
	if (!src) return gf_import_message(import, GF_URL_ERROR, "Opening file %s failed", import->in_name);

	switch (import->esd->decoderConfig->streamType) {
	case GF_STREAM_SCENE:
		mtype = GF_ISOM_MEDIA_SCENE;
		break;
	case GF_STREAM_VISUAL:
		mtype = GF_ISOM_MEDIA_VISUAL;
		break;
	case GF_STREAM_AUDIO:
		mtype = GF_ISOM_MEDIA_AUDIO;
		break;
	case GF_STREAM_TEXT:
		mtype = GF_ISOM_MEDIA_TEXT;
		break;
	case GF_STREAM_MPEG7:
		mtype = GF_ISOM_MEDIA_MPEG7;
		break;
	case GF_STREAM_IPMP:
		mtype = GF_ISOM_MEDIA_IPMP;
		break;
	case GF_STREAM_OCI:
		mtype = GF_ISOM_MEDIA_OCI;
		break;
	case GF_STREAM_MPEGJ:
		mtype = GF_ISOM_MEDIA_MPEGJ;
		break;
	case GF_STREAM_INTERACT:
		mtype = GF_STREAM_SCENE;
		break;
	/*not sure about this one...*/
	case GF_STREAM_IPMP_TOOL:
		mtype = GF_ISOM_MEDIA_IPMP;
		break;
	/*not sure about this one...*/
	case GF_STREAM_FONT:
		mtype = GF_ISOM_MEDIA_MPEGJ;
		break;
	default:
		mtype = GF_ISOM_MEDIA_ESM;
	}
	timescale = import->esd->slConfig ? import->esd->slConfig->timestampResolution : 1000;
	track = gf_isom_new_track(import->dest, import->esd->ESID, mtype, timescale);
	if (!track) {
		e = gf_isom_last_error(import->dest);
		goto exit;
	}
	gf_isom_set_track_enabled(import->dest, track, 1);
	if (!import->esd->ESID) import->esd->ESID = gf_isom_get_track_id(import->dest, track);
	import->final_trackID = import->esd->ESID;
	e = gf_isom_new_mpeg4_description(import->dest, track, import->esd, (import->flags & GF_IMPORT_USE_DATAREF) ? import->in_name : NULL, NULL, &di);
	if (e) goto exit;

	gf_import_message(import, GF_OK, "Raw Access Unit import (StreamType %s)", gf_odf_stream_type_name(import->esd->decoderConfig->streamType));

	samp = gf_isom_sample_new();
	gf_fseek(src, 0, SEEK_END);
	assert(gf_ftell(src) < 1<<31);
	samp->dataLength = (u32) gf_ftell(src);
	gf_fseek(src, 0, SEEK_SET);
	samp->IsRAP = RAP;
	samp->data = (char *)gf_malloc(sizeof(char)*samp->dataLength);
	read = (u32) fread(samp->data, sizeof(char), samp->dataLength, src);
	if ( read != samp->dataLength ) {
		e = gf_import_message(import, GF_IO_ERR, "Failed to read raw unit %d bytes", samp->dataLength);
		goto exit;

	}
	e = gf_isom_add_sample(import->dest, track, di, samp);
	gf_isom_sample_del(&samp);
	gf_media_update_bitrate(import->dest, track);
exit:
	gf_fclose(src);
	return e;
}


static GF_Err gf_import_vobsub(GF_MediaImporter *import)
{
#ifndef GPAC_DISABLE_VOBSUB
	static const u8 null_subpic[] = { 0x00, 0x09, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0xFF };
	char		  filename[GF_MAX_PATH];
	FILE		 *file = NULL;
	int		  version;
	vobsub_file	  *vobsub = NULL;
	u32		  c, trackID, track, di;
	Bool		  destroy_esd = GF_FALSE;
	GF_Err		  err = GF_OK;
	GF_ISOSample	 *samp = NULL;
	GF_List		 *subpic;
	u64 last_dts = 0;
	u32 total, last_samp_dur = 0;
	unsigned char buf[0x800];

	strcpy(filename, import->in_name);
	vobsub_trim_ext(filename);
	strcat(filename, ".idx");

	file = gf_fopen(filename, "r");
	if (!file) {
		err = gf_import_message(import, GF_URL_ERROR, "Opening file %s failed", filename);
		goto error;
	}

	GF_SAFEALLOC(vobsub, vobsub_file);
	if (!vobsub) {
		err = gf_import_message(import, GF_OUT_OF_MEM, "Memory allocation failed");
		goto error;
	}

	err = vobsub_read_idx(file, vobsub, &version);
	gf_fclose(file);

	if (err != GF_OK) {
		err = gf_import_message(import, err, "Reading VobSub file %s failed", filename);
		goto error;
	} else if (version < 6) {
		err = gf_import_message(import, err, "Unsupported VobSub version", filename);
		goto error;
	}

	if (import->flags & GF_IMPORT_PROBE_ONLY) {
		import->nb_tracks = 0;
		for (c = 0; c < 32; c++) {
			if (vobsub->langs[c].id != 0) {
				import->tk_info[import->nb_tracks].track_num = c + 1;
				import->tk_info[import->nb_tracks].media_oti = GPAC_OTI_MEDIA_SUBPIC;
				import->tk_info[import->nb_tracks].stream_type = GF_STREAM_VISUAL;
				import->tk_info[import->nb_tracks].flags	 = 0;
				import->nb_tracks++;
			}
		}
		vobsub_free(vobsub);
		return GF_OK;
	}

	strcpy(filename, import->in_name);
	vobsub_trim_ext(filename);
	strcat(filename, ".sub");

	file = gf_fopen(filename, "rb");
	if (!file) {
		err = gf_import_message(import, GF_URL_ERROR, "Opening file %s failed", filename);
		goto error;
	}

	trackID = import->trackID;
	if (!trackID) {
		trackID = 0-1U;
		if (vobsub->num_langs != 1) {
			err = gf_import_message(import, GF_BAD_PARAM, "Several tracks in VobSub - please indicate track to import");
			goto error;
		}
		for (c = 0; c < 32; c++) {
			if (vobsub->langs[c].id != 0) {
				trackID = c;
				break;
			}
		}
		if (trackID == 0-1U) {
			err = gf_import_message(import, GF_URL_ERROR, "Cannot find track ID %d in file", trackID);
			goto error;
		}
	}
	trackID--;

	if (!import->esd) {
		import->esd = gf_odf_desc_esd_new(2);
		destroy_esd = GF_TRUE;
	}
	if (!import->esd->decoderConfig) {
		import->esd->decoderConfig = (GF_DecoderConfig*)gf_odf_desc_new(GF_ODF_DCD_TAG);
	}
	if (!import->esd->slConfig) {
		import->esd->slConfig = (GF_SLConfig*)gf_odf_desc_new(GF_ODF_SLC_TAG);
	}
	if (!import->esd->decoderConfig->decoderSpecificInfo) {
		import->esd->decoderConfig->decoderSpecificInfo = (GF_DefaultDescriptor*)gf_odf_desc_new(GF_ODF_DSI_TAG);
	}

	import->esd->decoderConfig->streamType		 = GF_STREAM_ND_SUBPIC;
	import->esd->decoderConfig->objectTypeIndication = GPAC_OTI_MEDIA_SUBPIC;

	import->esd->decoderConfig->decoderSpecificInfo->dataLength = sizeof(vobsub->palette);
	import->esd->decoderConfig->decoderSpecificInfo->data		= (char*)&vobsub->palette[0][0];

	gf_import_message(import, GF_OK, "VobSub import - subpicture stream '%s'", vobsub->langs[trackID].name);

	track = gf_isom_new_track(import->dest, import->esd->ESID, GF_ISOM_MEDIA_SUBPIC, 90000);
	if (!track) {
		err = gf_isom_last_error(import->dest);
		err = gf_import_message(import, err, "Could not create new track");
		goto error;
	}

	gf_isom_set_track_enabled(import->dest, track, 1);

	if (!import->esd->ESID) {
		import->esd->ESID = gf_isom_get_track_id(import->dest, track);
	}
	import->final_trackID = import->esd->ESID;

	gf_isom_new_mpeg4_description(import->dest, track, import->esd, NULL, NULL, &di);
	gf_isom_set_track_layout_info(import->dest, track, vobsub->width << 16, vobsub->height << 16, 0, 0, 0);
	gf_isom_set_media_language(import->dest, track, vobsub->langs[trackID].name);

	samp = gf_isom_sample_new();
	samp->IsRAP = SAP_TYPE_1;
	samp->dataLength = sizeof(null_subpic);
	samp->data	= (char*)null_subpic;

	subpic = vobsub->langs[trackID].subpos;
	total  = gf_list_count(subpic);

	last_dts = 0;
	for (c = 0; c < total; c++)
	{
		u32		i, left, size, psize, dsize, hsize, duration;
		char *packet;
		vobsub_pos *pos = (vobsub_pos*)gf_list_get(subpic, c);

		if (import->duration && pos->start > import->duration) {
			break;
		}

		gf_fseek(file, pos->filepos, SEEK_SET);
		if (gf_ftell(file) != pos->filepos) {
			err = gf_import_message(import, GF_IO_ERR, "Could not seek in file");
			goto error;
		}

		if (!fread(buf, sizeof(buf), 1, file)) {
			err = gf_import_message(import, GF_IO_ERR, "Could not read from file");
			goto error;
		}

		if (*(u32*)&buf[0x00] != 0xba010000		   ||
		        *(u32*)&buf[0x0e] != 0xbd010000		   ||
		        !(buf[0x15] & 0x80)				   ||
		        (buf[0x17] & 0xf0) != 0x20			   ||
		        (buf[buf[0x16] + 0x17] & 0xe0) != 0x20)
		{
			gf_import_message(import, GF_CORRUPTED_DATA, "Corrupted data found in file %s", filename);
			continue;
		}

		psize = (buf[buf[0x16] + 0x18] << 8) + buf[buf[0x16] + 0x19];
		dsize = (buf[buf[0x16] + 0x1a] << 8) + buf[buf[0x16] + 0x1b];
		packet = (char *) gf_malloc(sizeof(char)*psize);
		if (!packet) {
			err = gf_import_message(import, GF_OUT_OF_MEM, "Memory allocation failed");
			goto error;
		}

		for (i = 0, left = psize; i < psize; i += size, left -= size) {
			hsize = 0x18 + buf[0x16];
			size  = MIN(left, 0x800 - hsize);
			memcpy(packet + i, buf + hsize, size);

			if (size != left) {
				while (fread(buf, 1, sizeof(buf), file)) {
					if (buf[buf[0x16] + 0x17] == (trackID | 0x20)) {
						break;
					}
				}
			}
		}

		if (i != psize || left > 0) {
			gf_import_message(import, GF_CORRUPTED_DATA, "Corrupted data found in file %s", filename);
			continue;
		}

		if (vobsub_get_subpic_duration(packet, psize, dsize, &duration) != GF_OK) {
			gf_import_message(import, GF_CORRUPTED_DATA, "Corrupted data found in file %s", filename);
			continue;
		}

		last_samp_dur = duration;

		/*first sample has non-0 DTS, add an empty one*/
		if (!c && (pos->start != 0)) {
			err = gf_isom_add_sample(import->dest, track, di, samp);
			if (err) goto error;
		}

		samp->data	 = packet;
		samp->dataLength = psize;
		samp->DTS	 = pos->start * 90;

		if (last_dts && (last_dts >= samp->DTS)) {
			err = gf_import_message(import, GF_CORRUPTED_DATA, "Out of order timestamps in vobsub file");
			goto error;
		}

		err = gf_isom_add_sample(import->dest, track, di, samp);
		if (err) goto error;
		gf_free(packet);

		gf_set_progress("Importing VobSub", c, total);
		last_dts = samp->DTS;

		if (import->flags & GF_IMPORT_DO_ABORT) {
			break;
		}
	}

	gf_isom_set_last_sample_duration(import->dest, track, last_samp_dur);

	gf_media_update_bitrate(import->dest, track);
	gf_set_progress("Importing VobSub", total, total);

	err = GF_OK;

error:
	if (import->esd && destroy_esd) {
		import->esd->decoderConfig->decoderSpecificInfo->data = NULL;
		gf_odf_desc_del((GF_Descriptor *)import->esd);
		import->esd = NULL;
	}
	if (samp) {
		samp->data = NULL;
		gf_isom_sample_del(&samp);
	}
	if (vobsub) {
		vobsub_free(vobsub);
	}
	if (file) {
		gf_fclose(file);
	}

	return err;
#else
	return GF_NOT_SUPPORTED;
#endif
}

GF_EXPORT
GF_Err gf_media_import_chapters_file(GF_MediaImporter *import)
{
	s32 read=0;
	GF_Err e;
	u32 state, offset;
	u32 cur_chap;
	u64 ts;
	u32 i, h, m, s, ms, fr, fps;
	char line[1024];
	char szTitle[1024];
	FILE *f = gf_fopen(import->in_name, "rt");
	if (!f) return GF_URL_ERROR;

	read = (s32) fread(line, 1, 4, f);
	if (read < 0) {
		e = GF_IO_ERR;
		goto err_exit;
	}
	if (read < 4) {
		e = GF_URL_ERROR;
		goto err_exit;
	}

	if ((line[0]==(char)(0xFF)) && (line[1]==(char)(0xFE))) {
		if (!line[2] && !line[3]) {
			e = GF_NOT_SUPPORTED;
			goto err_exit;
		}
		offset = 2;
	} else if ((line[0]==(char)(0xFE)) && (line[1]==(char)(0xFF))) {
		if (!line[2] && !line[3]) {
			e = GF_NOT_SUPPORTED;
			goto err_exit;
		}
		offset = 2;
	} else if ((line[0]==(char)(0xEF)) && (line[1]==(char)(0xBB)) && (line[2]==(char)(0xBF))) {
		/*we handle UTF8 as asci*/
		offset = 3;
	} else {
		offset = 0;
	}
	gf_fseek(f, offset, SEEK_SET);

	if (import->flags & GF_IMPORT_PROBE_ONLY) {
		Bool is_chap_or_sub = GF_FALSE;
		import->nb_tracks = 0;
		while (!is_chap_or_sub && (fgets(line, 1024, f) != NULL)) {
			char *sep;
			strlwr(line);

			if (strstr(line, "addchapter(")) is_chap_or_sub = GF_TRUE;
			else if (strstr(line, "-->")) is_chap_or_sub = GF_TRUE;
			else if ((sep = strstr(line, "chapter")) != NULL) {
				sep+=7;
				if (!strncmp(sep+1, "name", 4)) is_chap_or_sub = GF_TRUE;
				else if (!strncmp(sep+2, "name", 4)) is_chap_or_sub = GF_TRUE;
				else if (!strncmp(sep+3, "name", 4)) is_chap_or_sub = GF_TRUE;
				else if (strstr(line, "Zoom") || strstr(line, "zoom")) is_chap_or_sub = GF_TRUE;
			}
		}
		gf_fclose(f);
		if (is_chap_or_sub) {
			import->nb_tracks = 1;
			import->tk_info[0].media_4cc = GF_MEDIA_TYPE_CHAP;
			import->tk_info[0].stream_type = GF_STREAM_TEXT;
			return GF_OK;
		}
		return GF_NOT_SUPPORTED;
	}

	e = gf_isom_remove_chapter(import->dest, 0, 0);
	if (e) goto err_exit;

	if (!import->video_fps) {
		/*try to figure out the frame rate*/
		for (i=0; i<gf_isom_get_track_count(import->dest); i++) {
			GF_ISOSample *samp;
			u32 ts, inc;
			if (gf_isom_get_media_type(import->dest, i+1) != GF_ISOM_MEDIA_VISUAL) continue;
			if (gf_isom_get_sample_count(import->dest, i+1) < 20) continue;
			samp = gf_isom_get_sample_info(import->dest, 1, 2, NULL, NULL);
			inc = (u32) samp->DTS;
			if (!inc) inc=1;
			ts = gf_isom_get_media_timescale(import->dest, i+1);
			import->video_fps = ts;
			import->video_fps /= inc;
			gf_isom_sample_del(&samp);
			GF_LOG(GF_LOG_INFO, GF_LOG_AUTHOR, ("[Chapter import] Guessed video frame rate %g (%u:%u)\n", import->video_fps, ts, inc));
			break;
		}
		if (!import->video_fps)
			import->video_fps = 25;
	}

	cur_chap = 0;
	ts = 0;
	state = 0;
	while (fgets(line, 1024, f) != NULL) {
		char *title = NULL;
		u32 off = 0;
		char *sL;
		while (1) {
			u32 len = (u32) strlen(line);
			if (!len) break;
			switch (line[len-1]) {
			case '\n':
			case '\t':
			case '\r':
			case ' ':
				line[len-1] = 0;
				continue;
			}
			break;
		}

		while (line[off]==' ') off++;
		if (!strlen(line+off)) continue;
		sL = line+off;

		szTitle[0] = 0;
		/*ZoomPlayer chapters*/
		if (!strnicmp(sL, "AddChapter(", 11)) {
			u32 nb_fr;
			sscanf(sL, "AddChapter(%u,%s)", &nb_fr, szTitle);
			ts = nb_fr;
			ts *= 1000;
			ts = (u64) (((s64) ts ) / import->video_fps);
			sL = strchr(sL, ',');
			strcpy(szTitle, sL+1);
			sL = strrchr(szTitle, ')');
			if (sL) sL[0] = 0;
		} else if (!strnicmp(sL, "AddChapterBySecond(", 19)) {
			u32 nb_s;
			sscanf(sL, "AddChapterBySecond(%u,%s)", &nb_s, szTitle);
			ts = nb_s;
			ts *= 1000;
			sL = strchr(sL, ',');
			strcpy(szTitle, sL+1);
			sL = strrchr(szTitle, ')');
			if (sL) sL[0] = 0;
		} else if (!strnicmp(sL, "AddChapterByTime(", 17)) {
			u32 h, m, s;
			sscanf(sL, "AddChapterByTime(%u,%u,%u,%s)", &h, &m, &s, szTitle);
			ts = 3600*h + 60*m + s;
			ts *= 1000;
			sL = strchr(sL, ',');
			if (sL) sL = strchr(sL+1, ',');
			if (sL) sL = strchr(sL+1, ',');
			if (sL) strcpy(szTitle, sL+1);
			sL = strrchr(szTitle, ')');
			if (sL) sL[0] = 0;
		}
		/*regular or SMPTE time codes*/
		else if ((strlen(sL)>=8) && (sL[2]==':') && (sL[5]==':')) {
			title = NULL;
			if (strlen(sL)==8) {
				sscanf(sL, "%02u:%02u:%02u", &h, &m, &s);
				ts = (h*3600 + m*60+s)*1000;
			}
			else {
				char szTS[20], *tok;
				strncpy(szTS, sL, 18);
				tok = strrchr(szTS, ' ');
				if (tok) {
					title = strchr(sL, ' ') + 1;
					while (title[0]==' ') title++;
					if (strlen(title)) strcpy(szTitle, title);
					tok[0] = 0;
				}
				ts = 0;
				h = m = s = ms = 0;

				if (sscanf(szTS, "%u:%u:%u;%u/%u", &h, &m, &s, &fr, &fps)==5) {
					ts = (h*3600 + m*60+s)*1000 + 1000*fr/fps;
				} else if (sscanf(szTS, "%u:%u:%u;%u", &h, &m, &s, &fr)==4) {
					ts = (h*3600 + m*60+s);
					ts = (s64) (((import->video_fps*((s64)ts) + fr) * 1000 ) / import->video_fps);
				} else if (sscanf(szTS, "%u:%u:%u.%u", &h, &m, &s, &ms) == 4) {
					ts = (h*3600 + m*60+s)*1000+ms;
				} else if (sscanf(szTS, "%u:%u:%u.%u", &h, &m, &s, &ms) == 4) {
					ts = (h*3600 + m*60+s)*1000+ms;
				} else if (sscanf(szTS, "%u:%u:%u:%u", &h, &m, &s, &ms) == 4) {
					ts = (h*3600 + m*60+s)*1000+ms;
				} else if (sscanf(szTS, "%u:%u:%u", &h, &m, &s) == 3) {
					ts = (h*3600 + m*60+s) * 1000;
				}
			}
		}
		/*CHAPTERX= and CHAPTERXNAME=*/
		else if (!strnicmp(sL, "CHAPTER", 7)) {
			u32 idx;
			char szTemp[20], *str;
			strncpy(szTemp, sL, 19);
			str = strrchr(szTemp, '=');
			if (!str) continue;
			str[0] = 0;
			strlwr(szTemp);
			idx = cur_chap;
			str = strchr(sL, '=');
			str++;
			if (strstr(szTemp, "name")) {
				sscanf(szTemp, "chapter%uname", &idx);
				strcpy(szTitle, str);
				if (idx!=cur_chap) {
					cur_chap=idx;
					state = 0;
				}
				state++;
			} else {
				sscanf(szTemp, "chapter%u", &idx);
				if (idx!=cur_chap) {
					cur_chap=idx;
					state = 0;
				}
				state++;

				ts = 0;
				h = m = s = ms = 0;
				if (sscanf(str, "%u:%u:%u.%u", &h, &m, &s, &ms) == 4) {
					ts = (h*3600 + m*60+s)*1000+ms;
				} else if (sscanf(str, "%u:%u:%u:%u", &h, &m, &s, &ms) == 4) {
					ts = (h*3600 + m*60+s)*1000+ms;
				} else if (sscanf(str, "%u:%u:%u", &h, &m, &s) == 3) {
					ts = (h*3600 + m*60+s) * 1000;
				}
			}
			if (state==2) {
				e = gf_isom_add_chapter(import->dest, 0, ts, szTitle);
				if (e) goto err_exit;
				state = 0;
			}
			continue;
		}
		else continue;

		if (strlen(szTitle)) {
			e = gf_isom_add_chapter(import->dest, 0, ts, szTitle);
		} else {
			e = gf_isom_add_chapter(import->dest, 0, ts, NULL);
		}
		if (e) goto err_exit;
	}


err_exit:
	gf_fclose(f);
	return e;
}

GF_EXPORT
GF_Err gf_media_import_chapters(GF_ISOFile *file, char *chap_file, Double import_fps)
{
	GF_MediaImporter import;
	memset(&import, 0, sizeof(GF_MediaImporter));
	import.dest = file;
	import.in_name = chap_file;
	import.video_fps = import_fps;
	import.streamFormat = "CHAP";
	return gf_media_import(&import);
}

void on_import_setup_failure(GF_Filter *f, void *on_setup_error_udta, GF_Err e)
{
	GF_MediaImporter *importer = (GF_MediaImporter *)on_setup_error_udta;
	importer->last_error = e;
}

GF_EXPORT
GF_Err gf_media_import(GF_MediaImporter *importer)
{
#ifndef GPAC_DISABLE_TTXT
	GF_Err gf_import_timed_text(GF_MediaImporter *import);
#endif
	GF_Err e;
	u32 i, count;
	GF_FilterSession *fsess;
	GF_Filter *prober, *src_filter;
	char *ext, *xml_type;
	char *fmt = "";
	if (!importer || (!importer->dest && (importer->flags!=GF_IMPORT_PROBE_ONLY)) || (!importer->in_name && !importer->orig) ) return GF_BAD_PARAM;

	if (importer->orig) return gf_import_isomedia(importer);

	if (importer->force_ext) {
		ext = importer->force_ext;
	} else {
		ext = strrchr(importer->in_name, '.');
		if (!ext) ext = "";
	}

	if (importer->streamFormat) fmt = importer->streamFormat;


	/*always try with MP4 - this allows using .m4v extension for both raw CMP and iPod's files*/
	if (gf_isom_probe_file(importer->in_name)) {
		importer->orig = gf_isom_open(importer->in_name, GF_ISOM_OPEN_READ, NULL);
		if (importer->orig) {
			e = gf_import_isomedia(importer);
			gf_isom_delete(importer->orig);
			importer->orig = NULL;
			return e;
		}
	}
	/*old specific importers not remapped to filter session*/
	/*raw importer*/
	if (!stricmp(fmt, "RAW")) {
		return gf_import_raw_unit(importer);
	}
	/*SC3DMC*/
	if (!strnicmp(ext, ".s3d", 4) || !stricmp(fmt, "SC3DMC") )
		return gf_import_afx_sc3dmc(importer, GF_TRUE);

#ifdef FILTER_FIXME
	#error "importer TODO: SAF, TS"
#endif

	e = GF_OK;
	fsess = gf_fs_new(0, GF_FS_SCHEDULER_LOCK_FREE, NULL, GF_FALSE);
	importer->last_error = GF_OK;

	if (importer->flags & GF_IMPORT_PROBE_ONLY) {
		prober = gf_fs_load_filter(fsess, "probe:pid_only");

		src_filter = gf_fs_load_source(fsess, importer->in_name, NULL, NULL, &e);
		if (e) {
			gf_fs_del(fsess);
			return gf_import_message(importer, e, "[Importer] Cannot load filter for input file \"%s\"", importer->in_name);
		}
		gf_filter_set_setup_failure_callback(prober, src_filter, on_import_setup_failure, importer);
		gf_fs_run(fsess);

		if (importer->last_error) {
			gf_fs_del(fsess);
			return gf_import_message(importer, importer->last_error, "[Importer] Error probing %s", importer->in_name);
		}

		importer->nb_tracks = 0;
		count = gf_filter_get_ipid_count(prober);
		for (i=0; i<count; i++) {
			const GF_PropertyValue *p;
			struct __track_import_info *tki = &importer->tk_info[importer->nb_tracks];
			GF_FilterPid *pid = gf_filter_get_ipid(prober, i);

			p = gf_filter_pid_get_property(pid, GF_PROP_PID_STREAM_TYPE);
			tki->stream_type = p ? p->value.uint : GF_STREAM_UNKNOWN;
			p = gf_filter_pid_get_property(pid, GF_PROP_PID_OTI);
			tki->media_oti = p ? p->value.uint : GPAC_OTI_FORBIDDEN;
			//todo
			tki->flags=0;
			p = gf_filter_pid_get_property(pid, GF_PROP_PID_LANGUAGE);
			if (p && p->value.string) tki->lang = GF_4CC(p->value.string[0], p->value.string[1], p->value.string[2], ' ');
			p = gf_filter_pid_get_property(pid, GF_PROP_PID_ID);
			tki->track_num = p ? p->value.uint : 1;
			p = gf_filter_pid_get_property(pid, GF_PROP_PID_ESID);
			if (p) tki->mpeg4_es_id = p->value.uint;
			p = gf_filter_pid_get_property(pid, GF_PROP_PID_SERVICE_ID);
			if (p) tki->prog_num = p->value.uint;

			p = gf_filter_pid_get_property(pid, GF_PROP_PID_DURATION);
			if (p) {
				Double d = 1000 * p->value.frac.num;
				if (p->value.frac.den) d /= p->value.frac.den;
				if (d > importer->probe_duration) importer->probe_duration = (u64) d;
			}

			p = gf_filter_pid_get_property(pid, GF_PROP_PID_WIDTH);
			if (p) {
				tki->video_info.width = p->value.uint;
				p = gf_filter_pid_get_property(pid, GF_PROP_PID_HEIGHT);
				if (p) tki->video_info.height = p->value.uint;
				p = gf_filter_pid_get_property(pid, GF_PROP_PID_FPS);
				if (p) {
					tki->video_info.FPS = p->value.frac.num;
					if (p->value.frac.den) tki->video_info.FPS /= p->value.frac.den;
				}
				p = gf_filter_pid_get_property(pid, GF_PROP_PID_SAR);
				if (p) tki->video_info.par = (p->value.frac.num << 16) | p->value.frac.den;
			}
			p = gf_filter_pid_get_property(pid, GF_PROP_PID_SAMPLE_RATE);
			if (p) {
				tki->audio_info.sample_rate = p->value.uint;
				p = gf_filter_pid_get_property(pid, GF_PROP_PID_NUM_CHANNELS);
				if (p) tki->audio_info.nb_channels = p->value.uint;
				p = gf_filter_pid_get_property(pid, GF_PROP_PID_SAMPLES_PER_FRAME);
				if (p) tki->audio_info.samples_per_frame = p->value.uint;
			}
			p = gf_filter_pid_get_property(pid, GF_PROP_PID_CAN_DATAREF);

			if (p && p->value.boolean) tki->flags |= GF_IMPORT_USE_DATAREF;

			importer->nb_tracks++;
		}
	} else {
		char szArgs[4096];
		char szSubArg[1024];
		GF_Filter *isobmff_mux;

		//mux args
		sprintf(szArgs, "mp4mx:mov=%p:verbose", importer->dest);
		if (importer->flags & GF_IMPORT_FORCE_MPEG4) strcat(szArgs, ":m4sys:mpeg4");
		if (importer->flags & GF_IMPORT_USE_DATAREF) strcat(szArgs, ":dref");
		if (importer->flags & GF_IMPORT_NO_EDIT_LIST) strcat(szArgs, ":noedit");

		if (importer->duration) {
			sprintf(szSubArg, ":dur=%d/1000", importer->duration);
			strcat(szArgs, szSubArg);
		}
		if (importer->frames_per_sample) {
			sprintf(szSubArg, ":pack3gp=%d", importer->frames_per_sample);
			strcat(szArgs, szSubArg);
		}

		isobmff_mux = gf_fs_load_filter(fsess, szArgs);
		if (!isobmff_mux) {
			gf_fs_del(fsess);
			return gf_import_message(importer, GF_FILTER_NOT_FOUND, "[Importer] Cannot load ISOBMFF muxer");
		}

		//source args
		strcpy(szArgs, "");
		if (importer->flags & GF_IMPORT_SBR_IMPLICIT) strcat(szArgs, ":sbr=imp");
		else if (importer->flags & GF_IMPORT_SBR_EXPLICIT) strcat(szArgs, ":sbr=exp");
		if (importer->flags & GF_IMPORT_PS_IMPLICIT) strcat(szArgs, ":ps=imp");
		else if (importer->flags & GF_IMPORT_PS_EXPLICIT) strcat(szArgs, ":ps=exp");
		if (importer->flags & GF_IMPORT_OVSBR) strcat(szArgs, ":ovsbr");

		gf_fs_load_source(fsess, importer->in_name, szArgs, NULL, &e);
		if (e) {
			gf_fs_del(fsess);
			return gf_import_message(importer, e, "[Importer] Cannot load filter for input file \"%s\"", importer->in_name);
		}
		gf_fs_run(fsess);
		if (!importer->last_error) importer->last_error = gf_fs_get_last_connect_error(fsess);
		if (!importer->last_error) importer->last_error = gf_fs_get_last_process_error(fsess);

		if (importer->last_error) {
			gf_fs_del(fsess);
			return gf_import_message(importer, importer->last_error, "[Importer] Error probing %s", importer->in_name);
		}

		importer->final_trackID = gf_isom_get_last_created_track_id(importer->dest);
	}
	gf_fs_del(fsess);
	return GF_OK;

#if FILTER_FIXME

#ifndef GPAC_DISABLE_AVILIB
	/*AVI audio/video*/
	if (!strnicmp(ext, ".avi", 4) || !stricmp(fmt, "AVI") ) {
		e = gf_import_avi_video(importer);
		if (e) return e;
		return gf_import_avi_audio(importer);
	}
#endif

#ifndef GPAC_DISABLE_MPEG2PS
	/*MPEG PS*/
	if (!strnicmp(ext, ".mpg", 4) || !strnicmp(ext, ".mpeg", 5)
	        || !strnicmp(ext, ".vob", 4) || !strnicmp(ext, ".vcd", 4) || !strnicmp(ext, ".svcd", 5)
	        || !stricmp(fmt, "MPEG1") || !stricmp(fmt, "MPEG-PS")  || !stricmp(fmt, "MPEG2-PS")
	   ) {
		e = gf_import_mpeg_ps_video(importer);
		if (e) return e;
		return gf_import_mpeg_ps_audio(importer);
	}
#endif

#ifndef GPAC_DISABLE_AV_PARSERS
	/*H264/AVC video*/
	if (!strnicmp(ext, ".h264", 5) || !strnicmp(ext, ".264", 4) || !strnicmp(ext, ".x264", 5)
	        || !strnicmp(ext, ".h26L", 5) || !strnicmp(ext, ".26l", 4) || !strnicmp(ext, ".avc", 4)
	        || !stricmp(fmt, "AVC") || !stricmp(fmt, "H264") )
		return gf_import_avc_h264(importer);
	/*HEVC video*/
	if (!strnicmp(ext, ".hevc", 5) || !strnicmp(ext, ".hvc", 4) || !strnicmp(ext, ".265", 4) || !strnicmp(ext, ".h265", 5)
		|| !strnicmp(ext, ".shvc", 5) || !strnicmp(ext, ".lhvc", 5) || !strnicmp(ext, ".mhvc", 5)
	        || !stricmp(fmt, "HEVC") || !stricmp(fmt, "SHVC") || !stricmp(fmt, "MHVC") || !stricmp(fmt, "LHVC") || !stricmp(fmt, "H265") )
		return gf_import_hevc(importer);
#endif

	/*NHNT*/
	if (!strnicmp(ext, ".media", 5) || !strnicmp(ext, ".info", 5) || !strnicmp(ext, ".nhnt", 5) || !stricmp(fmt, "NHNT") )
		return gf_import_nhnt(importer);
	/*NHML*/
	if (!strnicmp(ext, ".nhml", 5) || !stricmp(fmt, "NHML") )
		return gf_import_nhml_dims(importer, GF_FALSE);
	/*text subtitles*/
	if (!strnicmp(ext, ".srt", 4) || !strnicmp(ext, ".sub", 4) || !strnicmp(ext, ".ttxt", 5) || !strnicmp(ext, ".vtt", 4) || !strnicmp(ext, ".ttml", 5)
	        || !stricmp(fmt, "SRT") || !stricmp(fmt, "SUB") || !stricmp(fmt, "TEXT") || !stricmp(fmt, "VTT") || !stricmp(fmt, "TTML")) {
#ifndef GPAC_DISABLE_TTXT
		return gf_import_timed_text(importer);
#else
		return GF_NOT_SUPPORTED;
#endif
	}
	/*VobSub*/
	if (!strnicmp(ext, ".idx", 4) || !stricmp(fmt, "VOBSUB"))
		return gf_import_vobsub(importer);

	/*DIMS*/
	if (!strnicmp(ext, ".dml", 4) || !stricmp(fmt, "DIMS") )
		return gf_import_nhml_dims(importer, GF_TRUE);

	if (!strnicmp(ext, ".txt", 4) || !strnicmp(ext, ".chap", 5) || !stricmp(fmt, "CHAP") )
		return gf_media_import_chapters_file(importer);

	if (!strnicmp(ext, ".swf", 4) || !strnicmp(ext, ".SWF", 4)) {
#ifndef GPAC_DISABLE_TTXT
		return gf_import_timed_text(importer);
#else
		return GF_NOT_SUPPORTED;
#endif
	}
	/*try XML things*/
	xml_type = gf_xml_get_root_type(importer->in_name, &e);
	if (xml_type) {
		if (!stricmp(xml_type, "TextStream") || !stricmp(xml_type, "text3GTrack") ) {
			gf_free(xml_type);
#ifndef GPAC_DISABLE_TTXT
			return gf_import_timed_text(importer);
#else
			return GF_NOT_SUPPORTED;
#endif
		}
		else if (!stricmp(xml_type, "NHNTStream")) {
			gf_free(xml_type);
			return gf_import_nhml_dims(importer, GF_FALSE);
		}
		else if (!stricmp(xml_type, "DIMSStream") ) {
			gf_free(xml_type);
			return gf_import_nhml_dims(importer, GF_TRUE);
		}
		gf_free(xml_type);
	}

	return gf_import_message(importer, e, "[Importer] Unknown input file type for \"%s\"", importer->in_name);


#endif


}



#endif /*GPAC_DISABLE_MEDIA_IMPORT*/


