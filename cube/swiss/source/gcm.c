#include <stdio.h>
#include <gccore.h>		/*** Wrapper to include common libogc headers ***/
#include <ogcsys.h>		/*** Needed for console support ***/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>
#include "gcm.h"
#include "main.h"
#include "util.h"
#include "dvd.h"
#include "swiss.h"
#include "cheats.h"
#include "patcher.h"
#include "sidestep.h"
#include "crc32/crc32.h"
#include "devices/deviceHandler.h"
#include "gui/FrameBufferMagic.h"
#include "gui/IPLFontWrite.h"
#include "../../reservedarea.h"
#include <sys/stat.h>
#include <errno.h>

#define FST_ENTRY_SIZE 12

// Parse disc header & read FST into a buffer
void* get_fst(file_handle* file) {
	DiskHeader header;
	char	*FST;
	
	// Grab disc header
	memset(&header,0,sizeof(DiskHeader));
	if(devices[DEVICE_CUR] == &__device_dvd) {
		if(DVD_Read(&header, 0, sizeof(DiskHeader)) != sizeof(DiskHeader)) {
			return NULL;
		}
	}
 	else {
		devices[DEVICE_CUR]->seekFile(file,0,DEVICE_HANDLER_SEEK_SET);
		if(devices[DEVICE_CUR]->readFile(file,&header,sizeof(DiskHeader)) != sizeof(DiskHeader)) {
			return NULL;
		}
	}
	
	if(header.DVDMagicWord != DVD_MAGIC) return NULL;
	
	// Alloc and read FST
	FST=(char*)memalign(32,header.FSTSize); 
	if(!FST) return NULL;

	if(devices[DEVICE_CUR] == &__device_dvd) {
		if(DVD_Read(FST, header.FSTOffset, header.FSTSize) != header.FSTSize) {
			free(FST);
			return NULL;
		}
	}
 	else {
		devices[DEVICE_CUR]->seekFile(file,header.FSTOffset,DEVICE_HANDLER_SEEK_SET);
		if(devices[DEVICE_CUR]->readFile(file,FST,header.FSTSize) != header.FSTSize) {
			free(FST); 
			return NULL;
		}
	}
	return FST;
}

// Populate the file_offset and file_size for searchFileName from the GCM FST
void get_fst_details(char *FST, char *searchFileName, u32 *file_offset, u32 *file_size) {
	u32 filename_offset, entries, string_table_offset, offset, i;
	char filename[256];
	// number of entries and string table location
	entries = *(unsigned int*)&FST[8];
	string_table_offset=12*entries; 
    
	// go through every entry
	for (i=1;i<entries;i++) 
	{ 
		offset=i*0x0c; 
		if(FST[offset]==0) //skip directories
		{ 
			filename_offset=(unsigned int)FST[offset+1]*256*256+(unsigned int)FST[offset+2]*256+(unsigned int)FST[offset+3]; 
			memset(filename,0,256);
			strcpy(filename,&FST[string_table_offset+filename_offset]); 
			if(!strcasecmp(filename,searchFileName)) 
			{
				memcpy(file_offset,&FST[offset+4],4);
				memcpy(file_size,&FST[offset+8],4);
				return;
			}		
		} 
	}
	*file_offset = 0;
	*file_size = 0;
}

//Lets parse the entire game FST in search for the banner
unsigned int getBannerOffset(file_handle *file) {
	char *FST = get_fst(file);
	if(!FST) return 0;
	
	u32 file_offset, file_size;
	get_fst_details(FST, "opening.bnr", &file_offset, &file_size);
	free(FST);
	return file_offset;
}

// Add a file to our current filesToPatch based on fileName
void parse_gcm_add(file_handle *file, ExecutableFile *filesToPatch, u32 *numToPatch, char *fileName) {
	char *FST = get_fst(file);
	if(!FST) return;
	
	u32 file_offset, file_size;
	get_fst_details(FST, fileName, &file_offset, &file_size);
	free(FST);
	if(file_offset != 0 && file_size != 0) {
		filesToPatch[*numToPatch].offset = file_offset;
		filesToPatch[*numToPatch].size = file_size;
		filesToPatch[*numToPatch].type = PATCH_OTHER;
		memcpy(&filesToPatch[*numToPatch].name,fileName,64); 
		*numToPatch += 1;
	}
}

// Returns the number of filesToPatch and fills out the filesToPatch array passed in (pre-allocated)
int parse_gcm(file_handle *file, ExecutableFile *filesToPatch) {

	char	*FST = get_fst(file);
	char	filename[256];
	int		numFiles = 0;

	if(!FST) return -1;

	u32 entries=*(unsigned int*)&FST[8];
	u32 string_table_offset=FST_ENTRY_SIZE*entries;
		
	int i;
	// go through every entry
	for (i=1;i<entries;i++) 
	{ 
		u32 offset=i*0x0c; 
		if(FST[offset]==0) //skip directories
		{ 
			u32 file_offset,size = 0;
			u32 filename_offset=((*(unsigned int*)&FST[offset]) & 0x00FFFFFF); 
			memset(&filename[0],0,256);
			memcpy(&filename[0],&FST[string_table_offset+filename_offset],255); 
			memcpy(&file_offset,&FST[offset+4],4);
			memcpy(&size,&FST[offset+8],4);
			if(endsWith(filename,".dol")) {
				// Some games contain a single "default.dol", these do not need 
				// pre-patching because they are what is actually pointed to by the apploader (and loaded by us)
				if(GCMDisk.DOLOffset == file_offset) {
					continue;
				}
				filesToPatch[numFiles].offset = file_offset;
				filesToPatch[numFiles].size = size;
				filesToPatch[numFiles].type = PATCH_DOL;
				memcpy(&filesToPatch[numFiles].name,&filename[0],64); 
				numFiles++;
			}
			if(endsWith(filename,".elf") && size < 12*1024*1024) {
				filesToPatch[numFiles].offset = file_offset;
				filesToPatch[numFiles].size = size;
				filesToPatch[numFiles].type = PATCH_ELF;
				memcpy(&filesToPatch[numFiles].name,&filename[0],64); 
				numFiles++;
			}
			if(strstr(filename,"execD.img")) {
				filesToPatch[numFiles].offset = file_offset;
				filesToPatch[numFiles].size = size;
				filesToPatch[numFiles].type = PATCH_LOADER;
				memcpy(&filesToPatch[numFiles].name,&filename[0],64); 
				numFiles++;
			}
			if(endsWith(filename,".tgc")) {
				// Go through all the TGC's internal files
				ExecutableFile *filesInTGCToPatch = memalign(32, sizeof(ExecutableFile)*32);
				int numTGCFilesToPatch = parse_tgc(file, filesInTGCToPatch, file_offset, filename), j;
				for(j=0; j<numTGCFilesToPatch; j++) {
					memcpy(&filesToPatch[numFiles], &filesInTGCToPatch[j], sizeof(ExecutableFile));
					numFiles++;
				}
				free(filesInTGCToPatch);
			}
		} 
	}
	free(FST);
	
	// Multi-DOL games may re-load the main DOL, so make sure we patch it too.
	if((devices[DEVICE_CUR]->features & FEAT_REPLACES_DVD_FUNCS) || numFiles > 0) {
		DOLHEADER dolhdr;
		u32 main_dol_size = 0;
		// Calc size
		devices[DEVICE_CUR]->seekFile(file,GCMDisk.DOLOffset,DEVICE_HANDLER_SEEK_SET);
		if(devices[DEVICE_CUR]->readFile(file,&dolhdr,DOLHDRLENGTH) != DOLHDRLENGTH) {
			DrawPublish(DrawMessageBox(D_FAIL, "Failed to read Main DOL Header"));
			while(1);
		}
		for (i = 0; i < MAXTEXTSECTION; i++) {
			if (dolhdr.textLength[i] + dolhdr.textOffset[i] > main_dol_size)
				main_dol_size = dolhdr.textLength[i] + dolhdr.textOffset[i];
		}
		for (i = 0; i < MAXDATASECTION; i++) {
			if (dolhdr.dataLength[i] + dolhdr.dataOffset[i] > main_dol_size)
				main_dol_size = dolhdr.dataLength[i] + dolhdr.dataOffset[i];
		}
		filesToPatch[numFiles].offset = GCMDisk.DOLOffset;
		filesToPatch[numFiles].size = main_dol_size;
		filesToPatch[numFiles].type = PATCH_DOL;
		sprintf(filesToPatch[numFiles].name, "Main DOL");
		numFiles++;
		
		// Patch the apploader too!
		// Calc Apploader trailer size
		u32 appldr_info[8];
		devices[DEVICE_CUR]->seekFile(file,0x2440,DEVICE_HANDLER_SEEK_SET);
		if(devices[DEVICE_CUR]->readFile(file,&appldr_info,32) != 32) {
			DrawPublish(DrawMessageBox(D_FAIL, "Failed to read Apploader info"));
			while(1);
		}
		filesToPatch[numFiles].size = appldr_info[6];
		filesToPatch[numFiles].offset = appldr_info[5] + 0x2460;
		filesToPatch[numFiles].type = appldr_info[0] == 0x32303034 ? PATCH_DOL:PATCH_LOADER;
		sprintf(filesToPatch[numFiles].name, "Apploader Trailer");
		numFiles++;
	}
	return numFiles;
}

// Adjust TGC FST entries in case we load a DOL from one directly
void adjust_tgc_fst(char* FST, u32 tgc_base, u32 fileAreaStart, u32 fakeAmount) {
	u32 entries=*(unsigned int*)&FST[8];
		
	int i;
	// go through every entry
	for (i=1;i<entries;i++) 
	{ 
		u32 offset=i*0x0c; 
		if(FST[offset]==0) //skip directories
		{ 
			u32 file_offset = 0;
			memcpy(&file_offset, &FST[offset+4], 4);
			*(unsigned int*)&FST[offset+4] = (file_offset-fakeAmount) + (tgc_base+fileAreaStart);
		} 
	}
}

int parse_tgc(file_handle *file, ExecutableFile *filesToPatch, u32 tgc_base, char* tgcname) {
	char	*FST; 
	char	filename[256];
	u32 fileAreaStart, fakeAmount, offset, size, numFiles = 0;
	
	// add this embedded GCM's main DOL
	devices[DEVICE_CUR]->seekFile(file,tgc_base+0x1C,DEVICE_HANDLER_SEEK_SET);
	devices[DEVICE_CUR]->readFile(file,&offset,4);
	devices[DEVICE_CUR]->readFile(file,&size,4);
	devices[DEVICE_CUR]->readFile(file, &fileAreaStart, 4);
	devices[DEVICE_CUR]->seekFile(file,tgc_base+0x34,DEVICE_HANDLER_SEEK_SET);
	devices[DEVICE_CUR]->readFile(file, &fakeAmount, 4);
	
	// Grab FST Offset & Size
	u32 fstOfsAndSize[2];
 	devices[DEVICE_CUR]->seekFile(file,tgc_base+0x10,DEVICE_HANDLER_SEEK_SET);
 	devices[DEVICE_CUR]->readFile(file,&fstOfsAndSize,2*sizeof(u32));
	
	// The TGC main DOL entry
	filesToPatch[numFiles].offset = offset+tgc_base;
	filesToPatch[numFiles].size = size;
	filesToPatch[numFiles].tgcFstOffset = tgc_base+fstOfsAndSize[0];
	filesToPatch[numFiles].tgcFstSize = fstOfsAndSize[1];
	filesToPatch[numFiles].tgcBase = tgc_base;
	filesToPatch[numFiles].tgcFileStartArea = fileAreaStart;
	filesToPatch[numFiles].tgcFakeOffset = fakeAmount;
	filesToPatch[numFiles].type = PATCH_DOL;
	sprintf(&filesToPatch[numFiles].name[0], "%s Main DOL", tgcname);
	numFiles++;

 	// Alloc and read FST
	FST=(char*)memalign(32,fstOfsAndSize[1]); 
	devices[DEVICE_CUR]->seekFile(file,tgc_base+fstOfsAndSize[0],DEVICE_HANDLER_SEEK_SET);
 	devices[DEVICE_CUR]->readFile(file,FST,fstOfsAndSize[1]);

	// Adjust TGC FST offsets
	adjust_tgc_fst(FST, tgc_base, fileAreaStart, fakeAmount);
	
	u32 entries=*(unsigned int*)&FST[8];
	u32 string_table_offset=FST_ENTRY_SIZE*entries;
		
	int i;
	// go through every entry
	for (i=1;i<entries;i++) 
	{ 
		u32 offset=i*0x0c; 
		if(FST[offset]==0) //skip directories
		{ 
			u32 file_offset,size = 0;
			u32 filename_offset=((*(unsigned int*)&FST[offset]) & 0x00FFFFFF); 
			memset(&filename[0],0,256);
			memcpy(&filename[0],&FST[string_table_offset+filename_offset],255); 
			memcpy(&file_offset,&FST[offset+4],4);
			memcpy(&size,&FST[offset+8],4);
			if(endsWith(filename,".dol")) {
				filesToPatch[numFiles].offset = file_offset;
				filesToPatch[numFiles].size = size;
				filesToPatch[numFiles].type = PATCH_DOL;
				filesToPatch[numFiles].tgcFstOffset = tgc_base+fstOfsAndSize[0];
				filesToPatch[numFiles].tgcFstSize = fstOfsAndSize[1];
				filesToPatch[numFiles].tgcBase = tgc_base;
				filesToPatch[numFiles].tgcFileStartArea = fileAreaStart;
				filesToPatch[numFiles].tgcFakeOffset = fakeAmount;
				memcpy(&filesToPatch[numFiles].name,&filename[0],64); 
				numFiles++;
			}
			if(endsWith(filename,".elf") && size < 12*1024*1024) {
				filesToPatch[numFiles].offset = file_offset;
				filesToPatch[numFiles].size = size;
				filesToPatch[numFiles].type = PATCH_ELF;
				memcpy(&filesToPatch[numFiles].name,&filename[0],64); 
				numFiles++;
			}
			if(strstr(filename,"execD.img")) {
				filesToPatch[numFiles].offset = file_offset;
				filesToPatch[numFiles].size = size;
				filesToPatch[numFiles].type = PATCH_LOADER;
				memcpy(&filesToPatch[numFiles].name,&filename[0],64); 
				numFiles++;
			}
		} 
	}
	free(FST);
	return numFiles;
}

int patch_gcm(file_handle *file, ExecutableFile *filesToPatch, int numToPatch, int multiDol) {
	int i, num_patched = 0;
	*(vu32*)VAR_EXECD_OFFSET = 0xFFFFFFFF;
	// If the current device isn't SD via EXI, init one slot to write patches.
	// TODO expand this to support IDE-EXI and other writable devices (requires dvd patch re-write/modularity)
	bool patchDeviceReady = false;
	if(!(devices[DEVICE_CUR]->features & FEAT_CAN_HOLD_PATCHES)) {
		if(deviceHandler_test(&__device_sd_c)) {
			devices[DEVICE_PATCHES] = &__device_sd_c;
		}
		else if(deviceHandler_test(&__device_sd_b)) {
			devices[DEVICE_PATCHES] = &__device_sd_b;
		}
		else if(deviceHandler_test(&__device_sd_a)) {
			devices[DEVICE_PATCHES] = &__device_sd_a;
		}
	}
	else {
		devices[DEVICE_PATCHES] = devices[DEVICE_CUR];
		patchDeviceReady = true;
	}
		
	if(devices[DEVICE_PATCHES] == NULL) {
		uiDrawObj_t *msgBox = DrawPublish(DrawMessageBox(D_FAIL, "No writable device present\nA SD Card Adapter must be inserted in\n order to utilise patches for this game."));
		sleep(5);
		DrawDispose(msgBox);
		return 0;
	}

	char patchDirName[256];
	char patchBaseDirName[256];
	char gameID[16];
	memset(&gameID, 0, 16);
	memset(&patchDirName, 0, 256);
	memset(&patchBaseDirName, 0, 256);
	strncpy((char*)&gameID, (char*)&GCMDisk, 4);
	sprintf(&patchDirName[0],"%sswiss_patches/%s",devices[DEVICE_PATCHES]->initial->name, &gameID[0]);
	memcpy((char*)&gameID, (char*)&GCMDisk, 12);
	sprintf(&patchBaseDirName[0],"%sswiss_patches",devices[DEVICE_PATCHES]->initial->name);
	print_gecko("Patch dir will be: %s if required\r\n", patchDirName);
	// Go through all the possible files we think need patching..
	for(i = 0; i < numToPatch; i++) {
		u32 patched = 0, crc32 = 0;

		sprintf(txtbuffer, "Patching File %i/%i",i+1,numToPatch);
			
		// Round up to 32 bytes
		if(filesToPatch[i].size % 0x20) {
			filesToPatch[i].size += (0x20-(filesToPatch[i].size%0x20));
		}
		
		if(filesToPatch[i].size > 8*1024*1024) {
			print_gecko("Skipping %s %iKB too large\r\n", filesToPatch[i].name, filesToPatch[i].size/1024);
			continue;
		}
		print_gecko("Checking %s %iKb\r\n", filesToPatch[i].name, filesToPatch[i].size/1024);
		if(strstr(filesToPatch[i].name, "execD.")) {
			*(vu32*)VAR_EXECD_OFFSET = filesToPatch[i].offset;
		}
		if(strstr(filesToPatch[i].name, "iwanagaD.dol") || strstr(filesToPatch[i].name, "switcherD.dol")) {
			continue;	// skip unused PSO files
		}
		uiDrawObj_t* progBox = DrawPublish(DrawProgressBar(true, 0, txtbuffer));
		int sizeToRead = filesToPatch[i].size, ret = 0;
		void *buffer = memalign(32, sizeToRead);
		
		devices[DEVICE_CUR]->seekFile(file,filesToPatch[i].offset, DEVICE_HANDLER_SEEK_SET);
		ret = devices[DEVICE_CUR]->readFile(file,buffer,sizeToRead);
		print_gecko("Read from %08X Size %08X - Result: %08X\r\n", filesToPatch[i].offset, sizeToRead, ret);
		if(ret != sizeToRead) {
			DrawDispose(progBox);			
			uiDrawObj_t *msgBox = DrawPublish(DrawMessageBox(D_FAIL, "Failed to read!"));
			sleep(5);
			DrawDispose(msgBox);
			return 0;
		}
		
		// Patch raw files for certain games
		if(filesToPatch[i].type == PATCH_OTHER) {
			patched += Patch_GameSpecificFile(buffer, sizeToRead, &gameID[0], filesToPatch[i].name);
		}
		else { 
			// Patch executable files
			if(devices[DEVICE_CUR]->features & FEAT_REPLACES_DVD_FUNCS) {
				if((devices[DEVICE_CUR]->features & FEAT_ALT_READ_PATCHES) || swissSettings.alternateReadPatches) {
					Patch_DVDLowLevelReadAlt(buffer, sizeToRead, filesToPatch[i].type);
				}
				else {
					ret = Patch_DVDLowLevelRead(buffer, sizeToRead, filesToPatch[i].type);
					if(READ_PATCHED_ALL != ret)	{
						DrawDispose(progBox);	
						uiDrawObj_t *msgBox = DrawPublish(DrawMessageBox(D_FAIL, "Failed to find necessary functions for patching!"));
						sleep(5);
						DrawDispose(msgBox);
					}
					else
						patched += 1;
				}
			}
		
			// Patch specific game hacks
			if(devices[DEVICE_CUR] != &__device_dvd && devices[DEVICE_CUR] != &__device_wkf) {
				patched += Patch_GameSpecific(buffer, sizeToRead, &gameID[0], filesToPatch[i].type);
			}
			
			// Patch IGR
			if(swissSettings.igrType != IGR_OFF || swissSettings.invertCStick) {
				Patch_PADStatus(buffer, sizeToRead, filesToPatch[i].type);
			}
					
			if(swissSettings.debugUSB && usb_isgeckoalive(1) && !swissSettings.wiirdDebug) {
				patched += Patch_Fwrite(buffer, sizeToRead);
			}
			
			if(swissSettings.wiirdDebug || getEnabledCheatsSize() > 0) {
				Patch_CheatsHook(buffer, sizeToRead, filesToPatch[i].type);
			}
			
			if(devices[DEVICE_CUR] == &__device_dvd && is_gamecube()) {
				patched += Patch_DVDLowLevelReadForDVD(buffer, sizeToRead, filesToPatch[i].type);
				patched += Patch_DVDReset(buffer, sizeToRead);
			}
			
			if(devices[DEVICE_CUR] == &__device_wkf) {
				patched += Patch_DVDLowLevelReadForWKF(buffer, sizeToRead, filesToPatch[i].type);
			}
			
			patched += Patch_FontEncode(buffer, sizeToRead);
			
			if(!swissSettings.disableVideoPatches) {
				Patch_GameSpecificVideo(buffer, sizeToRead, gameID, filesToPatch[i].type);
				Patch_VideoMode(buffer, sizeToRead, filesToPatch[i].type);
			}
			
			if(swissSettings.forceWidescreen)
				Patch_Widescreen(buffer, sizeToRead, filesToPatch[i].type);
			if(swissSettings.forceAnisotropy)
				Patch_TexFilt(buffer, sizeToRead, filesToPatch[i].type);
		}
		if(patched) {
			if(!patchDeviceReady) {
				deviceHandler_setStatEnabled(0);
				if(!devices[DEVICE_PATCHES]->init(devices[DEVICE_PATCHES]->initial)) {
					deviceHandler_setStatEnabled(1);
					DrawDispose(progBox);
					return false;
				}
				deviceHandler_setStatEnabled(1);
				patchDeviceReady = true;
			}
			// File handle for a patch we might need to write
			file_handle patchFile;
			memset(&patchFile, 0, sizeof(file_handle));
		
			int res;
			// Make a base patches dir if we don't have one already
			if((res=f_mkdir(&patchBaseDirName[0])) != FR_OK) {
				if(res != FR_EXIST) {
					print_gecko("Warning: %i [%s]\r\n",res, patchBaseDirName);
				}
			}
			if((res=f_mkdir(&patchDirName[0])) != FR_OK) {
				if(res != FR_EXIST) {
					print_gecko("Warning: %i [%s]\r\n",res, patchDirName);
				}
			}
			sprintf(patchFile.name, "%s/%i",patchDirName, num_patched);

			// Work out the crc32
			crc32 = Crc32_ComputeBuf( 0, buffer, (u32) sizeToRead);

			// See if this file already exists, if it does, match crc
			if(!devices[DEVICE_PATCHES]->readFile(&patchFile, NULL, 0)) {
				//print_gecko("Old Patch exists\r\n");
				u32 oldCrc32 = 0;
				devices[DEVICE_PATCHES]->seekFile(&patchFile, patchFile.size-4, DEVICE_HANDLER_SEEK_SET);
				devices[DEVICE_PATCHES]->readFile(&patchFile, &oldCrc32, 4);
				if(oldCrc32 == crc32) {
					num_patched++;
					devices[DEVICE_PATCHES]->closeFile(&patchFile);
					free(buffer);
					print_gecko("CRC matched, no need to patch again\r\n");
					DrawDispose(progBox);
					continue;
				}
				else {
					devices[DEVICE_PATCHES]->deleteFile(&patchFile);
					print_gecko("CRC mismatch, writing patch again\r\n");
				}
			}
			// Otherwise, write a file out for this game with the patched buffer inside.
			print_gecko("Writing patch file: %s %i bytes (disc offset %08X)\r\n", patchFile.name, sizeToRead, filesToPatch[i].offset);
			devices[DEVICE_PATCHES]->seekFile(&patchFile, 0, DEVICE_HANDLER_SEEK_SET);
			devices[DEVICE_PATCHES]->writeFile(&patchFile, buffer, sizeToRead);
			u32 magic = SWISS_MAGIC;
			devices[DEVICE_PATCHES]->writeFile(&patchFile, &filesToPatch[i].offset, 4);
			devices[DEVICE_PATCHES]->writeFile(&patchFile, &filesToPatch[i].size, 4);
			devices[DEVICE_PATCHES]->writeFile(&patchFile, &magic, 4);
			devices[DEVICE_PATCHES]->writeFile(&patchFile, &crc32, 4);
			devices[DEVICE_PATCHES]->closeFile(&patchFile);
			num_patched++;
			DrawDispose(progBox);
		}
		free(buffer);
	}

	return num_patched;
}

u64 calc_fst_entries_size(char *FST) {
	
	u64 totalSize = 0LL;
	u32 entries=*(unsigned int*)&FST[8];
	int i;
	for (i=1;i<entries;i++)	// go through every entry
	{ 
		if(!FST[i*0x0c]) {	// Skip directories
			totalSize += *(u32*)&FST[(i*0x0c)+8];
		} 
	}
	return totalSize;
}

// Returns the number of filesToPatch and fills out the filesToPatch array passed in (pre-allocated)
int read_fst(file_handle *file, file_handle** dir, u64 *usedSpace) {

	print_gecko("Read dir for directory: %s\r\n",file->name);
	char 	*FST = get_fst(file);
	char	filename[PATHNAME_MAX];
	int		numFiles = 1, idx = 0;
	int		isRoot = !strcmp((const char*)&file->name[0], "dvd:/");
	
	if(!FST) return -1;
	
	// Get the space taken up by this disc
	if(usedSpace) *usedSpace = calc_fst_entries_size(FST);
		
	// Add the disc itself as a "file"
	*dir = calloc( numFiles * sizeof(file_handle), 1 );
	DVD_Read((*dir)[idx].name, 32, 128);
	strcat((*dir)[idx].name, ".gcm");
	(*dir)[idx].fileBase = 0;
	(*dir)[idx].offset = 0;
	(*dir)[idx].size = DISC_SIZE;
	(*dir)[idx].fileAttrib = IS_FILE;
	(*dir)[idx].meta = 0;
	idx++;
	
	u32 entries=*(unsigned int*)&FST[8];
	u32 string_table_offset=FST_ENTRY_SIZE*entries;
		
	int i;
	// Go through the FST and find our DIR (or ROOT)
	int parent_dir_offset = (u32)file->fileBase;
	int dir_end_offset = isRoot ? *(u32*)&FST[8] : 0;
	isRoot = parent_dir_offset==0;
	
	// Haxx because we navigate GCM FST by offset and not name
	if(endsWith(file->name, "..") && strlen(file->name) > 2) {
		int len = strlen(file->name)-3;
		while(len && file->name[len-1]!='/')
      		len--;
		file->name[len-1] = 0;
	}
	
	u32 filename_offset=((*(u32*)&FST[parent_dir_offset*0x0C]) & 0x00FFFFFF); 
	memset(&filename[0],0,PATHNAME_MAX);
	sprintf(&filename[0], "%s/%s", file->name, &FST[string_table_offset+filename_offset]);
	dir_end_offset = *(u32*)&FST[(parent_dir_offset*0x0C) + 8];
	
	if(!isRoot) {
		// Add a special ".." dir which will take us back up a dir
		if(idx == numFiles){
			++numFiles;
			*dir = realloc( *dir, numFiles * sizeof(file_handle) ); 
		}
		memset(&(*dir)[idx], 0, sizeof(file_handle));
		sprintf((*dir)[idx].name, "%s/..", file->name);
		(*dir)[idx].fileBase = *(u32*)&FST[(parent_dir_offset*0x0C)+4];
		(*dir)[idx].offset = 0;
		(*dir)[idx].size = 0;
		(*dir)[idx].fileAttrib = IS_DIR;
		(*dir)[idx].meta = 0;
		idx++;
	}
	//print_gecko("Found DIR [%03i]:%s\r\n",parent_dir_offset,isRoot ? "ROOT":filename);
	
	// Traverse the FST now adding all the files which come under our DIR
	for (i=parent_dir_offset;i<dir_end_offset;i++) {
		if(i==0) continue;
		
		u32 offset=i*0x0c;
		u32 file_offset,size = 0;
		u32 filename_offset=((*(unsigned int*)&FST[offset]) & 0x00FFFFFF); 
		memset(&filename[0],0,PATHNAME_MAX);
		sprintf(&filename[0], "%s/%s", file->name, &FST[string_table_offset+filename_offset]);
		memcpy(&file_offset,&FST[offset+4],4);
		memcpy(&size,&FST[offset+8],4);
		
		// Is this a sub dir of our dir?
		if(FST[offset]) {
			if(file_offset == parent_dir_offset) {
				//print_gecko("Adding: [%03i]%s:%s offset %08X length %08X\r\n",i,!FST[offset] ? "File" : "Dir",filename,file_offset,size);
				if(idx == numFiles){
					++numFiles;
					*dir = realloc( *dir, numFiles * sizeof(file_handle) ); 
				}
				memset(&(*dir)[idx], 0, sizeof(file_handle));
				memcpy((*dir)[idx].name, &filename[0], PATHNAME_MAX);
				(*dir)[idx].fileBase = i;
				(*dir)[idx].offset = 0;
				(*dir)[idx].size = size;
				(*dir)[idx].fileAttrib = IS_DIR;
				(*dir)[idx].meta = 0;
				idx++;
				// Skip the entries that sit in this dir
				i = size-1;
			}
		}
		else {
			// File, add it.
			//print_gecko("Adding: [%03i]%s:%s offset %08X length %08X\r\n",i,!FST[offset] ? "File" : "Dir",filename,file_offset,size);
			if(idx == numFiles){
				++numFiles;
				*dir = realloc( *dir, numFiles * sizeof(file_handle) ); 
			}
			memset(&(*dir)[idx], 0, sizeof(file_handle));
			memcpy((*dir)[idx].name, &filename[0], PATHNAME_MAX);
			(*dir)[idx].fileBase = file_offset;
			(*dir)[idx].offset = 0;
			(*dir)[idx].size = size;
			(*dir)[idx].fileAttrib = IS_FILE;
			(*dir)[idx].meta = 0;
			idx++;
		}
	}
	
	free(FST);

	return numFiles;
}

// Returns the number of files matching the extension (in a TGC)
int parse_tgc_for_ext(file_handle *file, u32 tgc_base, char* tgcname, char* ext, bool find32k) {
	char	*FST; 
	char	filename[256];
	u32 fileAreaStart, fakeAmount, numFiles = 0;
	
	devices[DEVICE_CUR]->seekFile(file,tgc_base+0x24,DEVICE_HANDLER_SEEK_SET);
	devices[DEVICE_CUR]->readFile(file, &fileAreaStart, 4);
	devices[DEVICE_CUR]->seekFile(file,tgc_base+0x34,DEVICE_HANDLER_SEEK_SET);
	devices[DEVICE_CUR]->readFile(file, &fakeAmount, 4);
	
	// Grab FST Offset & Size
	u32 fstOfsAndSize[2];
 	devices[DEVICE_CUR]->seekFile(file,tgc_base+0x10,DEVICE_HANDLER_SEEK_SET);
 	devices[DEVICE_CUR]->readFile(file,&fstOfsAndSize,2*sizeof(u32));

 	// Alloc and read FST
	FST=(char*)memalign(32,fstOfsAndSize[1]); 
	devices[DEVICE_CUR]->seekFile(file,tgc_base+fstOfsAndSize[0],DEVICE_HANDLER_SEEK_SET);
 	devices[DEVICE_CUR]->readFile(file,FST,fstOfsAndSize[1]);

	// Adjust TGC FST offsets
	adjust_tgc_fst(FST, tgc_base, fileAreaStart, fakeAmount);
	
	u32 entries=*(unsigned int*)&FST[8];
	u32 string_table_offset=FST_ENTRY_SIZE*entries;
		
	int i;
	// go through every entry
	for (i=1;i<entries;i++) 
	{ 
		u32 offset=i*0x0c; 
		if(FST[offset]==0) //skip directories
		{ 
			u32 size = 0, file_offset = 0;
			u32 filename_offset=((*(unsigned int*)&FST[offset]) & 0x00FFFFFF); 
			memset(&filename[0],0,256);
			memcpy(&filename[0],&FST[string_table_offset+filename_offset],255);
			memcpy(&size,&FST[offset+8],4);
			memcpy(&file_offset,&FST[offset+4],4);
			// Match on file extension
			if(ext && size && endsWith(filename,ext)) {
				print_gecko("File matched ext (%s): %s/%s\r\n", ext, tgcname, filename);
				numFiles++;
			}
			// Match on file offset + size being a multiple of 32K
			if(find32k && size && !(size & 0x7FFF) && !(file_offset & 0x7FFF)) {
				print_gecko("File matched 32K size + alignment: %s/%s\r\n", tgcname, filename);
				numFiles++;
			}
		} 
	}
	free(FST);
	return numFiles;
}

// Returns the number of files matching the extension
int parse_gcm_for_ext(file_handle *file, char *ext, bool find32k) {

	char	*FST = get_fst(file);
	char	filename[256];
	int		numFiles = 0;

	if(!FST) return -1;

	u32 entries=*(unsigned int*)&FST[8];
	u32 string_table_offset=FST_ENTRY_SIZE*entries;
		
	int i;
	// go through every entry
	for (i=1;i<entries;i++) 
	{ 
		u32 offset=i*0x0c, file_offset = 0; 
		if(FST[offset]==0) //skip directories
		{ 
			u32 size = 0;
			u32 filename_offset=((*(unsigned int*)&FST[offset]) & 0x00FFFFFF); 
			memset(&filename[0],0,256);
			memcpy(&file_offset,&FST[offset+4],4);
			memcpy(&filename[0],&FST[string_table_offset+filename_offset],255);
			memcpy(&size,&FST[offset+8],4);
			// Match on file extension
			if(ext && endsWith(filename,ext) && size) {
				print_gecko("File matched ext (%s): %s\r\n", ext, filename);
				numFiles++;
			}
			// Match on file offset + size being a multiple of 32K
			if(find32k && size && !(size & 0x7FFF) && !(file_offset & 0x7FFF)) {
				print_gecko("File matched 32K size + alignment: %s\r\n", filename);
				numFiles++;
			}
			if(endsWith(filename,".tgc")) {
				// Go through all the TGC's internal files too
				numFiles += parse_tgc_for_ext(file, file_offset, filename, ext, find32k);
			}
		} 
	}
	free(FST);
	return numFiles;
}