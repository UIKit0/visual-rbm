#pragma once

#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#pragma warning (disable : 4482)

#pragma warning ( push )
#pragma warning (disable : 4480)

enum Endianness : uint16_t
{ 
	BigEndian = 0x0000,
	LittleEndian = 0xFFFF
};

enum DataFormat : uint8_t
{
	Invalid = 0x00,
	UInt8 = 0x08,
	SInt8 = 0x09,
	SInt16 = 0x0B,
	SInt32 = 0x0C,
	Single = 0x0D,
	Double = 0x0E
};

inline const Endianness SystemEndianness()
{
	union
	{
		uint32_t u32;
		uint16_t u16[2];
	};
	u32 = 0x0000FFFF;
	return (Endianness)u16[0];
}	

#pragma warning (pop)

class IDX
{
private:
	// info about the file
	Endianness _idx_endianness;
	DataFormat _data_format;
	uint32_t _row_dimensions_count;
	uint32_t* _row_dimensions;


	// bookkeeping data
	FILE* _idx_file;
	bool _writing;
	uint32_t _row_length;	// number of elements
	uint32_t _row_length_bytes;	// rnumber of bytes of each row
	void* _empty_row;

	uint32_t HeaderSize() const
	{
		return (2 + 1 + 1 + 4 * _row_dimensions_count);
	}

	void WriteHeader()
	{
		fseek(_idx_file, 0, SEEK_SET);

		// write out the header
		write<uint16_t>((uint16_t)_idx_endianness);
		write<uint8_t>((uint8_t)_data_format);
		write<uint8_t>((uint8_t)_row_dimensions_count);
		for(uint8_t k = 0; k < _row_dimensions_count; k++)
		{
			write<uint32_t>(_row_dimensions[k]);
		}
	}
	
	IDX() 
		: _idx_endianness(Endianness::BigEndian)
		, _data_format(DataFormat::Invalid)
		, _row_dimensions_count(0)
		, _row_dimensions(NULL)
		, _idx_file(NULL)
		, _writing(false)
		, _row_length(0)
		, _row_length_bytes(0)
		, _empty_row(NULL)
	{ }

#pragma region Read/Write Methods
	// binary reading methods
	template <typename T>
	T read()
	{
		T result;
		if(sizeof(T) != 1 && _idx_endianness != SystemEndianness())
		{
			uint8_t* buffer = (uint8_t*)&result;
			for(int i = sizeof(T) - 1; i >= 0; i--)
				buffer[i] = fgetc(_idx_file);
		}
		else
			fread(&result, sizeof(T), 1, _idx_file);

		return result;
	}

	template<typename T>
	T* read_row(T* row)
	{
		if(row)
		{
			if(sizeof(T) != 1 && _idx_endianness != SystemEndianness())
			{
				T val;
				uint8_t* byte_buffer = (uint8_t*)&val;
				for(uint32_t k = 0; k < _row_length; k++)
				{
					for(int i = sizeof(T) - 1; i >= 0; i--)
						byte_buffer[i] = fgetc(_idx_file);
					row[k] = val;
				}
			}
			else
				fread(row, sizeof(T), _row_length, _idx_file);
		}

		return row;
	}

	// binary writing methods
	template<typename T>
	void write(T t)
	{
		if(sizeof(T) != 1 && _idx_endianness != SystemEndianness())
		{
			uint8_t* buffer = (uint8_t*)&t;
			for(int i = sizeof(T) - 1; i >= 0; i--)
			{
				fputc(buffer[i], _idx_file);
			}
		}
		else
		{
			fwrite(&t, sizeof(t), 1, _idx_file);
		}
	}

	template<typename T>
	void write_row(T* row)
	{
		if(row)
		{
			if(sizeof(T) != 1 && _idx_endianness != SystemEndianness())
			{
				uint8_t* byte_buffer = (uint8_t*)row;
				for(uint32_t k = 0; k < _row_length; k++)
				{
					for(int i = sizeof(T) - 1; i >= 0; i--)
					{
						fputc(byte_buffer[i], _idx_file);
					}
					byte_buffer += sizeof(T);
				}
			}
			else
			{
				fwrite(row, sizeof(T), _row_length, _idx_file);
			}
		}
	}
#pragma endregion

public:


	~IDX()
	{
		if(_idx_file)
		{
			fclose(_idx_file);
			_idx_file = nullptr;
		}

		if(_empty_row)
		{
			free(_empty_row);
			_empty_row = NULL;
		}

		if(_row_dimensions)
		{
			free(_row_dimensions);
			_row_dimensions = NULL;
		}
	}

	static IDX* Load(const char* in_filename, bool in_writing=false)
	{
		FILE* file = fopen(in_filename, in_writing ? "rb+" : "rb");
		// make sure we could open the file
		if(file == NULL)
		{
			return NULL;
		}

		IDX* result = new IDX();
		IDX& idx = *result;

		idx._writing = in_writing;
		if(idx._writing)
		{
			idx._empty_row = malloc(idx._row_length_bytes);
			memset(idx._empty_row, 0x00, idx._row_length_bytes);
		}

		// start reading from file
		idx._idx_file = file;	
		idx._idx_endianness = (Endianness)idx.read<uint16_t>();

		// figure out our data format
		idx._data_format = (DataFormat)idx.read<uint8_t>();
		// read number of dimensions
		idx._row_dimensions_count = idx.read<uint8_t>();
		idx._row_dimensions = (uint32_t*)malloc(idx._row_dimensions_count * sizeof(uint32_t));
		
		// now get the dimensions list
		for(uint32_t k = 0; k < idx._row_dimensions_count; k++)
		{
			idx._row_dimensions[k] = idx.read<uint32_t>();
		}

		// first number is always the number of rows
		idx._row_length = 1;
		for(int k = 1; k < idx._row_dimensions_count; k++)
		{
			idx._row_length *= idx._row_dimensions[k];
		}

		switch(idx._data_format)
		{
		case DataFormat::UInt8:
		case DataFormat::SInt8:
			idx._row_length_bytes = idx._row_length;
			break;
		case DataFormat::SInt16:
			idx._row_length_bytes = idx._row_length * 2;
			break;
		case DataFormat::SInt32:
		case DataFormat::Single:
			idx._row_length_bytes = idx._row_length * 4;
			break;
		case DataFormat::Double:
			idx._row_length_bytes = idx._row_length * 8;
			break;
		}

		return result;	
	}

	static IDX* Create(const char* in_filename, Endianness in_endianness, DataFormat in_format, uint32_t row_length)
	{
		return Create(in_filename, in_endianness, in_format, &row_length, 1);

	}

	static IDX* Create(const char* in_filename, Endianness in_endianness, DataFormat in_format, uint32_t* row_dimensions, uint32_t row_dimensions_count)
	{
		FILE* file = fopen(in_filename, "wb");
		if(file == NULL)
		{
			return NULL;
		}

		IDX* result = new IDX();
		IDX& idx = *result;

		idx._writing = true;
		idx._empty_row = malloc(idx._row_length_bytes);
		memset(idx._empty_row, 0x00, idx._row_length_bytes);

		idx._idx_file = file;		
		idx._idx_endianness = in_endianness;

		idx._data_format = in_format;

		// read number of dimensions
		idx._row_dimensions_count = row_dimensions_count+1;  // +1 for row count
		idx._row_dimensions = (uint32_t*)malloc(idx._row_dimensions_count * sizeof(uint32_t));

		idx._row_dimensions[0] = 0;	// empty initially
		// get the dimensions list
		for(uint32_t k = 1; k < idx._row_dimensions_count; k++)
		{
			idx._row_dimensions[k] = row_dimensions[k-1];
		}

		idx._row_length = 1;
		for(int32_t i = 0; i < row_dimensions_count; i++)
		{
			idx._row_length *= row_dimensions[i];
		}

		switch(idx._data_format)
		{
		case DataFormat::UInt8:
		case DataFormat::SInt8:
			idx._row_length_bytes = idx._row_length;
			break;
		case DataFormat::SInt16:
			idx._row_length_bytes = idx._row_length * 2;
			break;
		case DataFormat::SInt32:
		case DataFormat::Single:
			idx._row_length_bytes = idx._row_length * 4;
			break;
		case DataFormat::Double:
			idx._row_length_bytes = idx._row_length * 8;
			break;
		}

		// write the header
		idx.WriteHeader();


		return result;
	}

	inline bool AddRow() 
	{
		return AddRows(1);
	};

	bool AddRows(uint32_t count)
	{
		if(!_writing)
		{
			return false;
		}

		// overflow
		if(GetRowCount() + count < GetRowCount())
		{
			return false;
		}

		// move to end of file
		if(fseek(_idx_file, 0, SEEK_END) != 0)
		{
			return false;
		}

		// write empty row to for each
		for(uint32_t k = 0; k < count; k++)
		{
			fwrite(_empty_row, _row_length_bytes, 1, _idx_file);
		}

		// flush data
		fflush(_idx_file);
		// increment number of rows
		_row_dimensions[0] += count;
		return true;
	}

	bool AddRow(const void* buffer)
	{
		if(!_writing)
		{
			// not writing
			return false;
		}

		// overflow
		if(GetRowCount() + 1 < GetRowCount())
		{
			return false;
		}

		// move to end of file
		if(fseek(_idx_file, 0, SEEK_END) != 0)
		{
			// could not seek
			return false;
		}

		// append the data
		switch(_data_format)
		{
		case DataFormat::UInt8:
		case DataFormat::SInt8:
			fwrite(buffer, 1, _row_length, _idx_file);
			break;
		case DataFormat::SInt16:
			write_row<int16_t>((int16_t*)buffer);
			break;
		case DataFormat::SInt32:
			write_row<int32_t>((int32_t*)buffer);
			break;
		case DataFormat::Single:
			write_row<float>((float*)buffer);
			break;
		case DataFormat::Double:
			write_row<double>((double*)buffer);
			break;
		}
		// flush the stream
		fflush(_idx_file);
		// increment number of rows
		_row_dimensions[0] += 1;
		return true;
	}

	bool WriteRow(uint32_t row, const void* buffer)
	{
		if(!_writing)
		{
			// not currently writing
			return false;
		}

		if(row >= GetRowCount())
		{
			// not a valid row
			return false;
		}


		if(fseek(_idx_file, HeaderSize() + row * _row_length_bytes, SEEK_SET) != 0)
		{
			// failed to seek to proper position
			return false;
		}
		
		// write to buffer
		switch(_data_format)
		{
		case DataFormat::UInt8:
		case DataFormat::SInt8:
			fwrite(buffer, 1, _row_length, _idx_file);
			break;
		case DataFormat::SInt16:
			write_row<int16_t>((int16_t*)buffer);
			break;
		case DataFormat::SInt32:
			write_row<int32_t>((int32_t*)buffer);
			break;
		case DataFormat::Single:
			write_row<float>((float*)buffer);
			break;
		case DataFormat::Double:
			write_row<double>((double*)buffer);
			break;
		}
		// flush the stream
		fflush(_idx_file);
		return true;
	}

	// read a given row to the passed in buffer
	bool ReadRow(uint32_t row, void* buffer)
	{
		if(row >= GetRowCount())
		{
			return false;
		}

		fseek(_idx_file, HeaderSize() + row * _row_length_bytes, SEEK_SET);

		switch(_data_format)
		{
		case DataFormat::UInt8:
		case DataFormat::SInt8:
			fread(buffer, 1, _row_length, _idx_file);
			break;
		case DataFormat::SInt16:
			read_row<int16_t>((int16_t*)buffer);
			break;
		case DataFormat::SInt32:
			read_row<int32_t>((int32_t*)buffer);
			break;
		case DataFormat::Single:
			read_row<float>((float*)buffer);
			break;
		case DataFormat::Double:
			read_row<double>((double*)buffer);
			break;
		}

		return true;
	}

	// writes the header and closes the underlying file
	bool Close()
	{
		if(_writing)
		{
			WriteHeader();
			// flush
			fflush(_idx_file);
		}


		// close
		fclose(_idx_file);

		_idx_file = NULL;

		return true;
	}

	inline uint32_t GetRowLength() const {return _row_length;}
	inline uint32_t GetRowLengthBytes() const {return _row_length_bytes;}
	inline DataFormat GetDataFormat() const {return _data_format;}
	inline uint32_t GetRowCount() const {return _row_dimensions[0];}
	inline uint32_t GetRowDimensionsCount() const {return _row_dimensions_count;}
	inline void GetRowDimensions(uint32_t* in_row_dimensions)
	{
		memcpy(in_row_dimensions, _row_dimensions, _row_dimensions_count * sizeof(uint32_t));
	}	
};
