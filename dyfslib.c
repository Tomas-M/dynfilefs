/*
   Functions used for dynfilefs
   Author: Tomas M <www.slax.org>
   License: GNU GPL
*/

// Find out first position where index can be written
//
static off_t find_first_empty_ix(off_t* curr_index)
{
   off_t start = *curr_index;
   off_t next_index = start + (NUM_INDEXED_BLOCKS * sizeof(off_t) * 2);
   off_t end = next_index - sizeof(off_t) * 2;
   off_t middle;
   off_t middle_offset;
   off_t middle_target;
   off_t result = 0;
   int ret;

   // check if next index is already known, in such case we'll just jump to it
   fseeko(fp, next_index, SEEK_SET);
   ret = fread(curr_index, sizeof(*curr_index), 1, fp);
   if (*curr_index != 0) return 0;
   *curr_index = start;

   while (start <= end)
   {
      // read block from the middle between start and end
      middle = (start + end) / 2;
      middle -= (middle - start) % (sizeof(off_t) * 2);

      fseeko(fp, middle, SEEK_SET);
      ret = fread(&middle_offset, sizeof(middle_offset), 1, fp);
      if (ret < 0) return 0;
      ret = fread(&middle_target, sizeof(middle_target), 1, fp);
      if (ret < 0) return 0;

      if (middle_offset == 0 && middle_target == 0) //empty block found, remember it
      {
         result = middle;
         end = middle;
      }
      else start = middle + sizeof(off_t) * 2;

      if (middle == start) break;
   }

   if (result == 0)
   {
      *curr_index = 0;
      fseeko(fp, next_index, SEEK_SET);
   }

   return result;
}


// find given offset in the current index block (start given by next_index)
//
static off_t find_offset(off_t* curr_index, off_t find_off)
{
   off_t start = *curr_index;
   off_t next_index = start + (NUM_INDEXED_BLOCKS * sizeof(off_t) * 2);
   off_t end = next_index - sizeof(off_t) * 2;
   off_t middle;
   off_t middle_offset;
   off_t middle_target;
   int ret;

   while (start <= end)
   {
      // read block from the middle between start and end
      middle = (start + end) / 2;
      middle -= (middle - start) % (sizeof(off_t) * 2);

      fseeko(fp, middle, SEEK_SET);
      ret = fread(&middle_offset, sizeof(middle_offset), 1, fp);
      if (ret < 0) return 0;
      ret = fread(&middle_target, sizeof(middle_target), 1, fp);
      if (ret < 0) return 0;

      if (middle_offset == find_off && middle_target != 0) // we found it!
         return middle_target;

      if (middle_target == 0) end = middle;
      else
      {
         if (middle_offset > find_off) end = middle - sizeof(off_t)*2;
         if (middle_offset < find_off) start = middle + sizeof(off_t)*2;
      }

      if (middle == start) break;
   }

   // if we are here, nothing was found
   fseeko(fp, next_index, SEEK_SET);
   ret = fread(curr_index, sizeof(*curr_index), 1, fp);

   return 0;
}

// discover real position of data for offset
//
static off_t get_data_offset(off_t offset)
{
   // align offset to block size
   offset -= offset % DATA_BLOCK_SIZE;

   off_t target = 0;
   off_t curr_index = first_index;

   while (target == 0 && curr_index != 0)
      target = find_offset(&curr_index, offset);

   return target;
}

// create empty index block at the end of storage file
// and write its position to wix_pos
//
static off_t create_new_index(off_t wix_pos)
{
   off_t next_index;
   next_index = first_index;
   int ret;

   fseeko(fp, 0, SEEK_END);
   next_index = ftello(fp);
   fseeko(fp, NUM_INDEXED_BLOCKS * sizeof(zero) * 2, SEEK_CUR);
   ret = fwrite(&zero, sizeof(zero), 1, fp);
   if (ret < 0) return 0;

   fseeko(fp, wix_pos, SEEK_SET);
   ret = fwrite(&next_index, sizeof(next_index), 1, fp);

   return next_index;
}


// find the best position of curr_offset in given index block
// so it could be relocated there (for sorting)
//
static off_t locate_best_position(off_t curr_index, off_t curr_pos, off_t curr_offset)
{
   off_t start = curr_index;
   off_t end = curr_pos;
   off_t middle_offset = 0;
   off_t middle;
   int ret;

   while (start != end)
   {
      // read block from the middle between start and end
      middle = (start + end) / 2;
      middle -= (middle - start) % (sizeof(curr_offset) * 2);

      fseeko(fp, middle, SEEK_SET);
      ret = fread(&middle_offset, sizeof(middle_offset), 1, fp);
      if (ret < 0) return 0;

      if (start == middle) // last two items
      {
         if (middle_offset > curr_offset) return start;
         else return end;
      }

      // update start or end accordingly
      if (middle_offset <= curr_offset) start = middle;
      if (middle_offset >= curr_offset) end = middle;
   }
   return end;
}

// given index block is already sorted expect one item
// find first unsorted item and sort it
// newly written item is at position curr_pos
//
static void sort_index_block(off_t curr_index, off_t curr_pos, off_t curr_offset, off_t curr_target)
{
   off_t puthere;
   size_t size;
   char * buf;
   int ret;

   // find seek position where new_val should be stored
   puthere = locate_best_position(curr_index, curr_pos, curr_offset);

   if (puthere != curr_pos)
   {
      // shift all data <puthere,curr_pos> forward
      // and write current offset+target at the puthere position
      size = curr_pos - puthere;
      fseeko(fp, puthere, SEEK_SET);
      buf = malloc(size); if (buf == NULL) return;
      ret = fread(buf, size, 1, fp); if (ret < 0) return;
      fseeko(fp, puthere, SEEK_SET);
      ret = fwrite(&curr_offset, sizeof(curr_offset), 1, fp);
      ret = fwrite(&curr_target, sizeof(curr_target), 1, fp);
      ret = fwrite(buf, size, 1, fp);
      free(buf);
   }

   return;
}

// create new data block and return its offset
//
static off_t create_data_offset(off_t offset)
{
   // align offset to block size
   offset -= offset % DATA_BLOCK_SIZE;

   off_t target;
   off_t ix = 0;
   off_t curr_index;
   int ret;

   curr_index = first_index;

   // find first available empty index position
   // keeps curr_index position set to the latest index block
   // or nulls it if new index has to be created, in such case ix=0 as well
   while (ix == 0 && curr_index != 0)
      ix = find_first_empty_ix(&curr_index);

   if (ix == 0 && curr_index == 0) // we reached end of index
   {
      ix = create_new_index(ftello(fp));
      curr_index = ix;
   }

   // write empty block entirely
   fseeko(fp, 0, SEEK_END);
   target = ftello(fp);
   ret = fwrite(&empty, DATA_BLOCK_SIZE, 1, fp);
   if (ret < 0) return 0;

   // write offset position to index
   fseeko(fp, ix, SEEK_SET);
   ret = fwrite(&offset, sizeof(offset), 1, fp);
   ret = fwrite(&target, sizeof(target), 1, fp);

   sort_index_block(curr_index, ix, offset, target);

   return target;
}
