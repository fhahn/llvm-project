target triple = "arm64-apple-macosx11.0.0"

%struct.png_struct_def = type { [48 x i32], ptr, ptr, i64, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, i8, i8, i32, i32, i32, i32, %struct.z_stream_s, ptr, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i64, i32, i32, i32, ptr, ptr, ptr, ptr, i64, i32, i32, ptr, i16, i32, i16, i8, i8, i8, i8, i8, i8, i8, i8, i8, i8, i8, i8, i8, i8, i8, i16, i8, i32, %struct.png_color_16_struct, %struct.png_color_16_struct, ptr, i32, i32, i32, i32, ptr, ptr, ptr, ptr, ptr, ptr, %struct.png_color_8_struct, %struct.png_color_8_struct, ptr, %struct.png_color_16_struct, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, i32, i32, i64, i64, i64, i64, i32, i32, ptr, ptr, i32, [29 x i8], i32, ptr, ptr, i32, i32, ptr, i8, i8, i16, i16, ptr, i32, i8, ptr, ptr, ptr, ptr, ptr, ptr, ptr, i8, i32, i32, i32, i64, %struct.png_unknown_chunk_t, i64, ptr, i64, i32, i32, ptr, [4 x ptr], %struct.png_colorspace }
%struct.z_stream_s = type { ptr, i32, i64, ptr, i32, i64, ptr, ptr, ptr, ptr, ptr, i32, i64, i64 }
%struct.png_color_8_struct = type { i8, i8, i8, i8, i8 }
%struct.png_color_16_struct = type { i8, i16, i16, i16, i16 }
%struct.png_unknown_chunk_t = type { [5 x i8], ptr, i64, i8 }
%struct.png_colorspace = type { i32, %struct.png_xy, %struct.png_XYZ, i16, i16 }
%struct.png_xy = type { i32, i32, i32, i32, i32, i32, i32, i32 }
%struct.png_XYZ = type { i32, i32, i32, i32, i32, i32, i32, i32, i32 }

define void @png_do_read_transformations(ptr noalias %png_ptr, ptr %0) {
entry:
  %add.ptr113 = getelementptr i8, ptr %0, i64 1
  %red407.i = getelementptr %struct.png_struct_def, ptr %png_ptr, i64 0, i32 74, i32 1
  %green410.i = getelementptr %struct.png_struct_def, ptr %png_ptr, i64 0, i32 74, i32 2
  %blue414.i = getelementptr %struct.png_struct_def, ptr %png_ptr, i64 0, i32 74, i32 3
  %add.ptr392.i = getelementptr i8, ptr %0, i64 2
  %add.ptr399.i = getelementptr i8, ptr %0, i64 3
  %1 = load i16, ptr %red407.i, align 2
  %conv408.i = trunc i16 %1 to i8
  store i8 %conv408.i, ptr %add.ptr113, align 1
  %2 = load i16, ptr %green410.i, align 4
  %conv411.i = trunc i16 %2 to i8
  store i8 %conv411.i, ptr %add.ptr392.i, align 1
  %3 = load i16, ptr %blue414.i, align 2
  %conv415.i = trunc i16 %3 to i8
  store i8 %conv415.i, ptr %add.ptr399.i, align 1
  ret void
}
