#include <iostream>
#include <string>
#include <fstream>
#include <math.h>
#include <vector>
#include <cstdint>
#include <cstring>


#ifdef __unix__
	#define OS_LINUX 1
	#include <unistd.h>
#else
	#define OS_LINUX 0
	#include <windows.h>
#endif


#define GRADIENT " .'`^\",:;Il!i><~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$"
#define MIN(a, b) (a < b) ? a : b
#define MAX(a, b) (a > b) ? a : b
#define TWOPOW(exp) (1 << exp)

/* TERMS
gct - global color table
lct - local color table
*/

/*Graphics Control Extension Format (8 bytes)

HEADER BYTES    /BYTE SIZE    /DELAY
       \_____   |       _____/  /TRANSPARENT COLOR INDEX
        21 F9   04  00  00 00  00  00 <TERMINATOR BYTE
                     \___________    
                      PACKED BYTE (read each bit) 
                      000 000 0 0   
                       |   |  | \Has Transparency
                       |   |  \Has User Input?
                       |   \Disposal Method
                       \Reserved Bits (future use)
*/
struct GCE { /*Graphics Control Extension*/
    int block_size_bytes;
    // within packed byte
    bool storage[3]; // 3 bits reserved for future use
    int disposal_method;
    bool user_input;
    bool transparent;

    int delay;
    int trans_color_idx;
};


/* Image Descriptor Format (10 bytes)

IMAGE HEADER            /IMAGE WIDTH
 /   LEFT   TOP   _____/ _____/IMAGE HEIGHT
2C  00 00  00 00  0A 00  0A 00  00
                     ___________/   
                     PACKED BYTE (read each bit) 
                     0 0 0 00 000   
             Has LCT?/ | |  |  |
         Is Interlaced?/ |  |  |
                  Sorted?/  |  |
  Reserved Bits (Future Use)/  |
                       LCT Size/

*/
struct ImgDesc { /*Image Descriptor*/
    int img_left;
    int img_top;
    int img_width;
    int img_height;

    bool has_lct;
    bool interlaced;
    bool sorted;
    bool storage[2]; // for future use
    int lct_size;
};


struct MiscExtend { /*For any unrecognized extensions*/
    // simply stores length of extension to skip
    // for now, used for plain text, application, and comment extensions
    int len_bytes;

};


struct Color {
    int red;
    int green;
    int blue;
};



int get_bits(unsigned src, int start, int numbits) {
    /* note: start is from the right*/
    unsigned long long int bitmask = 0;
    /* for every successive bit after the first,
        add an extra power of 2 to flip the next bit*/
    for (int i = 0; i < numbits; i++) {
        bitmask += TWOPOW(i);
    }
    /* first shift the input to the start bit
        then, bitwise-and with the bitmask to truncate
          any bits after the specified stop (numbits)*/
    return (src>>start) & bitmask;
}


int combine_bytes(char byte1, char byte2, bool little_endian = true) {
    // use bitshift to combine 2 bytes (8 bits each) into one 16 bit integer
    //  note: the bytes are concactenated in reverse to account for little endian notation
    return (
        ((unsigned short) (little_endian ? byte2 : byte1) << 8) |
        (unsigned char) (little_endian ? byte1 : byte2)
    );
}


char color_to_ascii(Color color) {
    float brightness = (color.red + color.green + color.blue)/3;
    return (GRADIENT[MAX(0,((int) (brightness/255 * sizeof(GRADIENT)))-1)]);
}


GCE make_GCE(char raw_bytes[]) {
    /*Return a Graphics Control Extension given its representation in raw bytes*/

    int byte_size = (uint8_t) raw_bytes[2];

    char packed = raw_bytes[3];
    // unpack 4th byte
    int disposal_method = get_bits(packed, 2, 3);
    bool has_user_input = packed & 0b00000010;
    bool has_transparency = packed & 0b00000001;

    int delay = combine_bytes(raw_bytes[4], raw_bytes[5]);
    int transparent_color_index = (uint8_t) raw_bytes[6];

    return GCE {
        byte_size, 
        {false, false, false}, // initially empty storage
        disposal_method,
        has_user_input, 
        has_transparency,
        delay,
        transparent_color_index,
    };
}


ImgDesc make_ImgDesc(char raw_bytes[]) {
    /*Return an Image Descriptor given its representation in raw bytes*/
    // ignore all but final byte

    int img_left = combine_bytes(raw_bytes[0], raw_bytes[1]);
    int img_top = combine_bytes(raw_bytes[2], raw_bytes[3]);
    int img_width = combine_bytes(raw_bytes[4], raw_bytes[5]);
    int img_height = combine_bytes(raw_bytes[6], raw_bytes[7]);

    char packed = raw_bytes[8];

    bool has_lct = packed & 0b10000000;
    bool interlaced = packed & 0b01000000;
    bool sorted = packed & 0b00100000;;
    int lct_size = TWOPOW(get_bits(packed, 0, 3)+1);

    return ImgDesc {
        img_left,
        img_top,
        img_width,
        img_height,
        has_lct,
        interlaced,
        sorted,
        {false, false},
        lct_size,
    };
}


void populate_color_table(std::ifstream& file, Color table[], int table_len) {
    // multiply by 3, as each color has a byte for R, G, and B
    int ct_size_bytes = table_len*3;
    char color_buf[ct_size_bytes]; 

    file.read(color_buf, ct_size_bytes);

    for (int i = 0; i < ct_size_bytes; i++) {
        switch (i%3) {
            case 0: 
                table[i/3].red = (uint8_t) color_buf[i];
                break;
            
            case 1: 
                table[i/3].green = (uint8_t) color_buf[i];
                break;
            
            case 2: 
                table[i/3].blue = (uint8_t) color_buf[i];
                break;
            
        }
    }
}


void decompress_image(
    char lzw_buf[], int lzw_len,
    int min_lzw, 
    int table_len,
    int index_buf[]
) {
    /*decompresses the LZW encoding used in GIF files
        use local color table if availiable*/

    int ibuf_index = 0;
    #define APPEND_INDEX(index) index_buf[ibuf_index++] = index

    unsigned long int bytes_evaluated = 0;

    int code_size = min_lzw + 1; // (In bits) + 1 to account for special codes

    std::vector<std::vector<int>> og_code_table;
    for (int i = 0; i < table_len+2; i++) {
        std::vector<int> inner;
        inner.push_back(i);
        og_code_table.push_back(inner);
    }

    std::vector<std::vector<int>> code_table;
    // two extra special codes at the end
    const int re_init_code = TWOPOW(min_lzw);
    const int end_of_info = TWOPOW(min_lzw) + 1;

    int table_end = end_of_info;

    #define INIT_CODE_TABLE() code_table = og_code_table; table_end = end_of_info
    #define APPEND_CODE_TABLE(entry) code_table.push_back(entry); table_end++
    
    // the codes being read are variable in length, but must fit
    // within 12 bits
    unsigned short cur_bits = combine_bytes(lzw_buf[0], lzw_buf[1]);
    bytes_evaluated += 2;
    // since a code can span across multiple bytes, keep track of
    // how many bits of the current byte have already been read 
    // as part of the previous code
    unsigned short cur_byte_offset = 0; 

    int prev_code = -1;
    int cur_code;

    bool is_special;
    int bits_evaluated;
    int bits;

    while (1) {
        // get code
        is_special = false;
        cur_code = get_bits(cur_bits, 0, code_size);
        if (cur_code == re_init_code) {
            INIT_CODE_TABLE();
            is_special = true;
            prev_code = -1;
            code_size = min_lzw + 1;
        } else if (cur_code == end_of_info) {
            break;
        } else {
            if (cur_code <= table_end) {
                for (int i : code_table[cur_code]) {
                    APPEND_INDEX(i);
                }
                if (prev_code != -1) {
                    std::vector<int> new_vec;
                    for (int i : code_table[prev_code]) {
                        new_vec.push_back(i);
                    }
                    new_vec.push_back(code_table[cur_code][0]);
                    APPEND_CODE_TABLE(new_vec);
                }
            } else {
                if (prev_code != -1) {
                    std::vector<int> new_vec;
                    for (int i : code_table[prev_code]) {
                        APPEND_INDEX(i);
                        new_vec.push_back(i);
                    }
                    APPEND_INDEX(code_table[prev_code][0]);
                    new_vec.push_back(code_table[prev_code][0]);
                    APPEND_CODE_TABLE(new_vec);
                }
            }
        }
        if (!is_special) 
            prev_code = cur_code;

        // get next bits
        bits_evaluated = 0;
        cur_bits = cur_bits >> code_size;
        while (bits_evaluated < code_size) {
            
            bits = MIN(code_size-bits_evaluated, 8-cur_byte_offset);

            cur_bits |= 
                (unsigned short) get_bits(lzw_buf[bytes_evaluated], cur_byte_offset, bits)
                << 16-code_size+bits_evaluated;

            bits_evaluated += bits;
            cur_byte_offset += bits;
            if (cur_byte_offset >= 8)
                bytes_evaluated++;
            cur_byte_offset %= 8;
        }
        
        if (table_end == (1 << code_size)-1 && code_size < 12) {
            code_size++;
        }

    }
}


void fill_frame(
    char frame[], 
    int index_stream[], int full_width, int full_height,
    Color table[], int table_len,
    GCE gce,
    ImgDesc img_desk
) {
    // convert color table to ascii format
    char ascii_table[table_len];
    for (int i = 0; i < table_len; i++) {
        ascii_table[i] = color_to_ascii(table[i]);
		}
    
		int size = full_height*full_width;

    int index = 0;
    for (int i = 0; i < size; i++) {
        if (
            (int) i / full_width < img_desk.img_top ||
            i / full_width >= img_desk.img_top + img_desk.img_height ||
            (int) i % full_width < img_desk.img_left ||
            i % full_width >= img_desk.img_left + img_desk.img_width
        ) {
            continue;
        } else {
            if (gce.transparent && gce.trans_color_idx == index_stream[index]) {
								index++;
								continue;
            }
            frame[i] = ascii_table[index_stream[index]];
            index++;
        }

    }
}


void print_frame(char frame[], int img_width, int img_height) {
    std::string final = "";
    for (int i = 0; i < img_height; i++) {
        if (i % 2) // skip every other row
            continue;
        for (int j = 0; j < img_width; j++) {
            final += frame[i*img_width + j];
				}
        final += '\n';
		}
		std::cout << final << '\n';

}


void raise_user_error(const std::string &err_msg, const std::string &prg_name) {
    std::cout << err_msg << '\n';
    std::cout << "  Usage: " << prg_name << " [gif filepath]\n";
}


int main(int argc, char *argv[]) {
  	
    if (argc < 2) {
        raise_user_error("Error: too few program arguments", argv[0]);
        return 1;
    }

    std::ifstream file;
    file.open(argv[1], std::ios_base::in | std::ios_base::binary);

    if (!file.is_open()) {
        raise_user_error("Error: .gif file not found", argv[0]);
        return 1;
    }

    // read header to ensure gif 87a or 89a version
    char head_buf[7] = {0};
    head_buf[6] = '\0'; // null byte at end for string comparison

    file.read(head_buf, 6);

    if (strcmp(head_buf, "GIF87a") != 0 && strcmp(head_buf, "GIF89a") != 0) { 
        raise_user_error("Error: bad data found in .gif file", argv[0]);
        return 1;
    }
    bool is_gif89 = (strcmp(head_buf, "GIF89a") == 0);

    // read logical screen descriptor
    /* Format:
                       
           HEIGHT     (last 2 bytes are redundant)
    F2 01  F2 01  F7  00 00
    WIDTH          \___________
                     PACKED BYTE (read each bit)
                     1 111 0 111
                     |  |  |  \global color table size
                     |  |  \is sorted
                     |  \color resolution
                     \has global color table
    */      
    char lsd_buf[7];
    file.read(lsd_buf, 7);

    unsigned short width  = combine_bytes(lsd_buf[0], lsd_buf[1]);
    unsigned short height = combine_bytes(lsd_buf[2], lsd_buf[3]);
    
		// unpack this byte
    unsigned char packed = lsd_buf[4];
    bool has_gct = false; // has global color table?
    short color_resolution;
    bool gct_sorted;
    int gct_size;

    char frame[height*width]; 

    // check individual bits using bitwise and operator
    if (packed & 0b10000000) {
        has_gct = true;
        color_resolution = get_bits(packed, 4, 3) + 1;
        gct_sorted = packed & 0b00001000;
        gct_size = TWOPOW(get_bits(packed, 0, 3)+1);
    }

    Color gct[gct_size];
    // populate global color table
    if (has_gct) 
        populate_color_table(file, gct, gct_size);
    
    int bg_color = lsd_buf[5];

    // after global color table, the contents of the file start
    char stack_buffer[256]; // buffer
    uint8_t stack_pointer = 0;
    char temp[1];
    
    #define READ_NEXT() file.read(temp, 1);stack_buffer[stack_pointer++] = temp[0]

    bool is_extension = false;
    GCE gce;

    ImgDesc cur_img_desc;
    Color lct[265];

    int min_lzw;

    char lzw_buf[width*height]; // encoded image data
    unsigned int img_pnt = 0;

    #define IMAGE_APPEND(chr) lzw_buf[img_pnt] = chr; img_pnt++
    int img_block_size;

    int index_stream[width*height];

    do {
        READ_NEXT();
        if (is_extension) {
            // read next byte to determine size of extension
            READ_NEXT();
            // check which extension it is with second byte
            switch (get_bits(stack_buffer[1], 0, 8)) {

                case (255): // Application extension (NETSCAPE 2.0)
                    // ignore this extension for now (next 16 bytes)
                    file.seekg(16, std::ios::cur);
                    break;
                
                case (254):  // Comment extension
                    // again, just ignore this for now
                    do {
                        file.seekg(get_bits(temp[0], 0, 8)+1, std::ios::cur);
                        READ_NEXT();
                    } while (get_bits(temp[0], 0, 8) != 0);
                    break;
                
                case (249):  // GCE
                    //read the remaining 5 bytes of GCE
                    for (int i = 0; i < 5; i++) {
                        READ_NEXT();
                    }
                    gce = make_GCE(stack_buffer);
                    break;
                
                case (1):  // Plain text extension (captions)
                    file.seekg(get_bits(temp[0], 0, 8), std::ios::cur);
                    // now keep reading until you hit the end of the text
                    do {
                        READ_NEXT();
                    } while (get_bits(temp[0], 0, 8) != 0);
                    break;
                
            };
            stack_pointer = 0;
            is_extension = false;
        } 
        switch (temp[0]) {
            case '!': // ! indicates special information (Extensions, Comments)
                is_extension = true;
                break;
            
            case ',': // , is the start of an image
                // image header
                stack_pointer = 0;
                for (int i = 0; i < 9; i++) {
                    READ_NEXT();
                }
                cur_img_desc = make_ImgDesc(stack_buffer);
                if (cur_img_desc.has_lct) 
                    populate_color_table(file, lct, cur_img_desc.lct_size);
                
                // start of image data
                READ_NEXT();
                min_lzw = get_bits(temp[0], 0, 8);

                do { // read image
                    READ_NEXT();
                    img_block_size = get_bits(temp[0], 0, 8);
                    for (int i = 0; i < img_block_size; i++) {
                        file.read(temp, 1);
                        IMAGE_APPEND(temp[0]);
                    }
                } while (img_block_size != 0);

                decompress_image(
                    lzw_buf, 
                    img_pnt-1, 
                    min_lzw, 
                    (cur_img_desc.has_lct ? cur_img_desc.lct_size : gct_size),
                    index_stream
                );

                fill_frame(
                    frame,
                    index_stream,
                    width,
                    height,
                    (cur_img_desc.has_lct ? lct : gct), 
                    (cur_img_desc.has_lct ? cur_img_desc.lct_size : gct_size),
                    gce,
                    cur_img_desc
                );

                print_frame(frame, width, height);

								#ifdef __unix__
									usleep((float) gce.delay*10*1000);	
								#else
									Sleep((float) gce.delay*10);
								#endif

								system(OS_LINUX ? "clear" : "cls");

                stack_pointer = 0;
                img_pnt = 0;
                break;
        }
    } while (temp[0] != ';'); // ; is the marker for end of file

    file.close();
    
    return 0;
}
