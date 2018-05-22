
// -----------------------------------------------------------------------------
// SLADE - It's a Doom Editor
// Copyright(C) 2008 - 2017 Simon Judd
//
// Email:       sirjuddington@gmail.com
// Web:         http://slade.mancubus.net
// Filename:    ADatArchive.cpp
// Description: ADatArchive, archive class to handle the Anachronox dat format
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 2 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110 - 1301, USA.
// -----------------------------------------------------------------------------


// -----------------------------------------------------------------------------
//
// Includes
//
// -----------------------------------------------------------------------------
#include "Main.h"
#include "ADatArchive.h"
#include "General/UI.h"
#include "Utility/Compression.h"


// -----------------------------------------------------------------------------
//
// Constants
//
// -----------------------------------------------------------------------------
#define DIRENTRY 144


// -----------------------------------------------------------------------------
//
// External Variables
//
// -----------------------------------------------------------------------------
EXTERN_CVAR(Bool, archive_load_data)


// -----------------------------------------------------------------------------
//
// ADatArchive Class Functions
//
// -----------------------------------------------------------------------------


// -----------------------------------------------------------------------------
// Reads dat format data from a MemChunk
// Returns true if successful, false otherwise
// -----------------------------------------------------------------------------
bool ADatArchive::open(MemChunk& mc)
{
	// Check given data is valid
	if (mc.size() < 16)
		return false;

	// Read dat header
	char magic[4];
	long dir_offset;
	long dir_size;
	mc.seek(0, SEEK_SET);
	mc.read(magic, 4);
	mc.read(&dir_offset, 4);
	mc.read(&dir_size, 4);

	// Check it
	if (magic[0] != 'A' || magic[1] != 'D' || magic[2] != 'A' || magic[3] != 'T')
	{
		Log::info(1, "ADatArchive::open: Opening failed, invalid header");
		Global::error = "Invalid dat header";
		return false;
	}

	// Stop announcements (don't want to be announcing modification due to entries being added etc)
	setMuted(true);

	// Read the directory
	size_t num_entries = dir_size / DIRENTRY;
	mc.seek(dir_offset, SEEK_SET);
	UI::setSplashProgressMessage("Reading dat archive data");
	for (uint32_t d = 0; d < num_entries; d++)
	{
		// Update splash window progress
		UI::setSplashProgress(((float)d / (float)num_entries));

		// Read entry info
		char name[128];
		long offset;
		long decsize;
		long compsize;
		long whatever; // No idea what this could be
		mc.read(name, 128);
		mc.read(&offset, 4);
		mc.read(&decsize, 4);
		mc.read(&compsize, 4);
		mc.read(&whatever, 4);

		// Byteswap if needed
		offset   = wxINT32_SWAP_ON_BE(offset);
		decsize  = wxINT32_SWAP_ON_BE(decsize);
		compsize = wxINT32_SWAP_ON_BE(compsize);

		// Check offset+size
		if ((unsigned)(offset + compsize) > mc.size())
		{
			Log::info(1, "ADatArchive::open: dat archive is invalid or corrupt (entry goes past end of file)");
			Global::error = "Archive is invalid and/or corrupt";
			setMuted(false);
			return false;
		}

		// Parse name
		wxFileName fn(wxString::FromAscii(name, 128));

		// Create directory if needed
		ArchiveTreeNode* dir = createDir(fn.GetPath(true, wxPATH_UNIX).ToStdString());

		// Create entry
		ArchiveEntry* entry       = new ArchiveEntry(fn.GetFullName().ToStdString(), compsize);
		entry->exProp("Offset")   = (int)offset;
		entry->exProp("FullSize") = (int)decsize;
		entry->setLoaded(false);
		entry->setState(0);

		// Add to directory
		dir->addEntry(entry);
	}

	// Detect all entry types
	MemChunk              edata;
	vector<ArchiveEntry*> all_entries;
	getEntryTreeAsList(all_entries);
	UI::setSplashProgressMessage("Detecting entry types");
	for (size_t a = 0; a < all_entries.size(); a++)
	{
		// Update splash window progress
		UI::setSplashProgress((((float)a / (float)num_entries)));

		// Get entry
		ArchiveEntry* entry = all_entries[a];

		// Read entry data if it isn't zero-sized
		if (entry->size() > 0)
		{
			// Read the entry data
			mc.exportMemChunk(edata, (int)entry->exProp("Offset"), entry->size());
			MemChunk xdata;
			if (Compression::ZlibInflate(edata, xdata, (int)entry->exProp("FullSize")))
				entry->importMemChunk(xdata);
			else
			{
				Log::info(fmt::format("Entry {} couldn't be inflated", entry->name()));
				entry->importMemChunk(edata);
			}
		}

		// Detect entry type
		EntryType::detectEntryType(entry);

		// Unload entry data if needed
		if (!archive_load_data)
			entry->unloadData();

		// Set entry to unchanged
		entry->setState(0);
	}

	// Setup variables
	setMuted(false);
	setModified(false);
	announce("opened");

	UI::setSplashProgressMessage("");

	return true;
}

// -----------------------------------------------------------------------------
// Writes the dat archive to a MemChunk
// Returns true if successful, false otherwise
// -----------------------------------------------------------------------------
bool ADatArchive::write(MemChunk& mc, bool update)
{
	// Clear current data
	mc.clear();
	MemChunk directory;
	MemChunk compressed;

	// Get archive tree as a list
	vector<ArchiveEntry*> entries;
	getEntryTreeAsList(entries);

	// Write header
	long     dir_offset = wxINT32_SWAP_ON_BE(16);
	long     dir_size   = wxINT32_SWAP_ON_BE(0);
	char     pack[4]    = { 'A', 'D', 'A', 'T' };
	uint32_t version    = wxINT32_SWAP_ON_BE(9);
	mc.seek(0, SEEK_SET);
	mc.write(pack, 4);
	mc.write(&dir_offset, 4);
	mc.write(&dir_size, 4);
	mc.write(&version, 4);

	// Write entry data
	for (auto& entry : entries)
	{
		// Skip folders
		if (entry->type() == EntryType::folderType())
			continue;

		// Create compressed version of the lump
		MemChunk* data = nullptr;
		if (Compression::ZlibDeflate(entry->data(), compressed, 9))
		{
			data = &compressed;
		}
		else
		{
			data = &(entry->data());
			Log::info(fmt::format("Entry {} couldn't be deflated", entry->name()));
		}

		// Update entry
		int offset = mc.currentPos();
		if (update)
		{
			entry->setState(0);
			entry->exProp("Offset") = (int)offset;
		}

		///////////////////////////////////
		// Step 1: Write directory entry //
		///////////////////////////////////

		// Check entry name
		string name = entry->path(true);
		name.erase(0, 1); // Remove leading /
		if (name.size() > 128)
		{
			Log::info(fmt::format(
				"Warning: Entry {} path is too long (> 128 characters), putting it in the root directory", name));
			wxFileName fn(name);
			name = fn.GetFullName();
			if (name.size() > 128)
				name = name.substr(0, 128);
		}

		// Write entry name
		char name_data[128];
		memset(name_data, 0, 128);
		memcpy(name_data, name.c_str(), name.size());
		directory.write(name_data, 128);

		// Write entry offset
		long myoffset = wxINT32_SWAP_ON_BE(offset);
		directory.write(&myoffset, 4);

		// Write full entry size
		long decsize = wxINT32_SWAP_ON_BE(entry->size());
		directory.write(&decsize, 4);

		// Write compressed entry size
		long compsize = wxINT32_SWAP_ON_BE(data->size());
		directory.write(&compsize, 4);

		// Write whatever it is that should be there
		// TODO: Reverse engineer what exactly it is and implement something valid for the game.
		long whatever = 0;
		directory.write(&whatever, 4);

		//////////////////////////////
		// Step 2: Write entry data //
		//////////////////////////////

		mc.write(data->data(), data->size());
	}

	// Write directory
	dir_offset = wxINT32_SWAP_ON_BE(mc.currentPos());
	dir_size   = wxINT32_SWAP_ON_BE(directory.size());
	mc.write(directory.data(), directory.size());

	// Update directory offset and size in header
	mc.seek(4, SEEK_SET);
	mc.write(&dir_offset, 4);
	mc.write(&dir_size, 4);

	// Yay! Finished!
	return true;
}

// -----------------------------------------------------------------------------
// Writes the dat archive to a file
// Returns true if successful, false otherwise
// -----------------------------------------------------------------------------
bool ADatArchive::write(string_view filename, bool update)
{
	// Write to a MemChunk, then export it to a file
	MemChunk mc;
	if (write(mc, true))
		return mc.exportFile(filename.data());
	else
		return false;
}

// -----------------------------------------------------------------------------
// Loads an entry's data from the dat file
// Returns true if successful, false otherwise
// -----------------------------------------------------------------------------
bool ADatArchive::loadEntryData(ArchiveEntry* entry)
{
	// Check entry is ok
	if (!checkEntry(entry))
		return false;

	// Do nothing if the entry's size is zero,
	// or if it has already been loaded
	if (entry->size() == 0 || entry->isLoaded())
	{
		entry->setLoaded();
		return true;
	}

	// Open archive file
	wxFile file(filename_);

	// Check it opened
	if (!file.IsOpened())
	{
		Log::info(fmt::format("ADatArchive::loadEntryData: Unable to open archive file {}", filename_));
		return false;
	}

	// Seek to entry offset in file and read it in
	file.Seek((int)entry->exProp("Offset"), wxFromStart);
	entry->importFileStream(file, entry->size());

	// Set the lump to loaded
	entry->setLoaded();

	return true;
}


// -----------------------------------------------------------------------------
//
// ADatArchive Class Static Functions
//
// -----------------------------------------------------------------------------


// -----------------------------------------------------------------------------
// Checks if the given data is a valid Anachronox dat archive
// -----------------------------------------------------------------------------
bool ADatArchive::isADatArchive(MemChunk& mc)
{
	// Check it opened ok
	if (mc.size() < 16)
		return false;

	// Read dat header
	char magic[4];
	long dir_offset;
	long dir_size;
	long version;
	mc.seek(0, SEEK_SET);
	mc.read(magic, 4);
	mc.read(&dir_offset, 4);
	mc.read(&dir_size, 4);
	mc.read(&version, 4);

	// Byteswap values for big endian if needed
	dir_size   = wxINT32_SWAP_ON_BE(dir_size);
	dir_offset = wxINT32_SWAP_ON_BE(dir_offset);

	// Check version
	if (wxINT32_SWAP_ON_BE(version) != 9)
		return false;

	// Check header
	if (magic[0] != 'A' || magic[1] != 'D' || magic[2] != 'A' || magic[3] != 'T')
		return false;

	// Check directory is sane
	if (dir_offset < 16 || (unsigned)(dir_offset + dir_size) > mc.size())
		return false;

	// That'll do
	return true;
}

// -----------------------------------------------------------------------------
// Checks if the file at [filename] is a valid Anachronox dat archive
// -----------------------------------------------------------------------------
bool ADatArchive::isADatArchive(string_view filename)
{
	// Open file for reading
	wxFile file(filename.data());

	// Check it opened ok
	if (!file.IsOpened() || file.Length() < 16)
		return false;

	// Read dat header
	char magic[4];
	long dir_offset;
	long dir_size;
	long version;
	file.Seek(0, wxFromStart);
	file.Read(magic, 4);
	file.Read(&dir_offset, 4);
	file.Read(&dir_size, 4);
	file.Read(&version, 4);

	// Byteswap values for big endian if needed
	dir_size   = wxINT32_SWAP_ON_BE(dir_size);
	dir_offset = wxINT32_SWAP_ON_BE(dir_offset);

	// Check version
	if (wxINT32_SWAP_ON_BE(version) != 9)
		return false;

	// Check header
	if (magic[0] != 'A' || magic[1] != 'D' || magic[2] != 'A' || magic[3] != 'T')
		return false;

	// Check directory is sane
	if (dir_offset < 16 || dir_offset + dir_size > file.Length())
		return false;

	// That'll do
	return true;
}
