#include <iostream>
#include <string.h>
#include <fstream>
#include <math.h>
#include <vector>

#define ASCII_GRADIENT " .\\'`^\",:;Il!i><~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$"

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
        bitmask += pow(2, i);
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
    int brightness = (color.red + color.green + color.blue)/3;
    return (ASCII_GRADIENT[brightness]);
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

    char packed = raw_bytes[9];

    bool has_lct = packed & 0b10000000;
    bool interlaced = packed & 0b01000000;
    bool sorted = packed & 0b00100000;;
    int lct_size = pow(2, get_bits(packed, 0, 3)+1);

    return ImgDesc {
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
    Color table[], int table_len,
    int img_width, int img_height,
    int index_buf[]
) {
    /*decompresses the LZW encoding used in GIF files
        use local color table if availiable*/

    int codes_evaluated = 0;
    #define APPEND_INDEX(index) index_buf[codes_evaluated++] = index

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
    #define INIT_CODE_TABLE() code_table = og_code_table

    const int re_init_code = pow(2, min_lzw);
    const int end_of_info = pow(2, min_lzw) + 1;
    
    // the codes being read are variable in length, but must fit
    // within 12 bits
    unsigned short cur_bits = combine_bytes(lzw_buf[0], lzw_buf[1]);
    bytes_evaluated += 2;
    // since a code can span across multiple bytes, keep track of
    // how many bits of the current byte have already been read 
    // as part of the previous code
    unsigned short cur_byte_offset = code_size % 8; 

    int prev_code;
    int cur_code;

    while (bytes_evaluated < lzw_len) {
        // get code
        cur_code = get_bits(cur_bits, 0, code_size);
        std::cout << cur_code <<'\n';
        std::cout << bytes_evaluated << '\n';
        std::cout << cur_byte_offset << "==\n";

        if (cur_code == re_init_code) {
            INIT_CODE_TABLE();
        } else if (cur_code == end_of_info) {
            break;
        } else {
            for (int i : code_table[cur_code])
                APPEND_INDEX(i);
        }
        prev_code = cur_code;

        // get next bits
        cur_bits = cur_bits >> code_size;
        cur_byte_offset = (cur_byte_offset + code_size) % 8;
        // at most, the next code can span across 3 bytes (depending on cur_offset and code size)
        cur_bits |= get_bits(lzw_buf[bytes_evaluated++], cur_byte_offset, 8) << 16-code_size;
        if (code_size > 8) {
            if (cur_byte_offset <= 3)
                cur_bits |= get_bits(lzw_buf[bytes_evaluated++], 0, 8) << code_size + 16 - cur_byte_offset;
            cur_bits |= get_bits(lzw_buf[bytes_evaluated++], 0, cur_byte_offset) << 16 - code_size + cur_byte_offset + 8;
        }

    }
}


void print_image(char image[], Color table[], int table_len) {

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
    std::cout << width << ' '<< height << '\n';
    // unpack this byte
    unsigned char packed = lsd_buf[4];
    bool has_gct = false; // has global color table?
    short color_resolution;
    bool gct_sorted;
    int gct_size;

    // check individual bits using bitwise and operator
    if (packed & 0b10000000) {
        has_gct = true;
        color_resolution = get_bits(packed, 4, 3) + 1;
        gct_sorted = packed & 0b00001000;
        gct_size = pow(2, get_bits(packed, 0, 3)+1);
    }

    Color gct[gct_size];
    // populate global color table
    if (has_gct) 
        populate_color_table(file, gct, gct_size);

    // after global color table, the contents of the file start
    char stack_buffer[256]; // buffer
    uint8_t stack_pointer = 0;
    char temp[1];
    
    #define READ_NEXT() file.read(temp, 1);stack_buffer[stack_pointer++] = temp[0]

    bool is_extension = false;
    GCE gce;

    ImgDesc cur_img_desc;
    Color lct[265];

    char lzw_buf[width*height]; // encoded image data
    unsigned int img_pnt = 0;

    #define IMAGE_APPEND(chr) lzw_buf[img_pnt] = chr; img_pnt++


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
                int min_lzw = get_bits(temp[0], 0, 8);

                int img_block_size;
                do {
                    READ_NEXT();
                    img_block_size = get_bits(temp[0], 0, 8);
                    for (int i = 0; i < img_block_size; i++) {
                        file.read(temp, 1);
                        IMAGE_APPEND(temp[0]);
                    }
                } while (img_block_size != 0);

                int index_stream[width*height];

                // std::cout << min_lzw; return 0;

                decompress_image(
                    lzw_buf, 
                    img_pnt-1, 
                    min_lzw, 
                    (cur_img_desc.has_lct ? lct : gct),
                    (cur_img_desc.has_lct ? cur_img_desc.lct_size : gct_size),
                    width, height,
                    index_stream
                );

                stack_pointer = 0;
                img_pnt = 0;

                break;
        }
    } while (temp[0] != ';'); // ; is the marker for end of file




    file.close();
    
    return 0;
}