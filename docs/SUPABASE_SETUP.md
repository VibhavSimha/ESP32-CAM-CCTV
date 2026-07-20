# Supabase Setup

This project uses Supabase Storage for motion-triggered cloud backup.

## Steps
1. Create a free account and new project on [Supabase](https://supabase.com/).
2. Navigate to **Storage** and create a new bucket named `cctv-clips`.
3. Disable "Public" access for this bucket.
4. Set up Row Level Security (RLS) policies for the bucket:
   - Allow `INSERT` and `UPDATE` operations for the `anon` key.
   - Alternatively, restrict inserts to specific paths like `events/*`.
5. Obtain your API credentials:
   - Go to **Project Settings** -> **API**.
   - Copy the **Project URL** and the **anon public** key.
6. Paste these credentials into your `config.h` file.

## Optional: Postgres Metadata Table
You can create a `motion_events` table in the database to log the triggers:

```sql
create table motion_events (
  id bigint generated always as identity primary key,
  frame_index int not null,
  file_path text not null,
  created_at timestamptz default now()
);
```
Uncomment the corresponding code block in `cloud_storage.cpp` to enable metadata logging.
