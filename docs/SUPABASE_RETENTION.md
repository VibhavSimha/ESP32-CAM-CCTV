# Supabase Cloud Retention — Server-Side FIFO (pg_cron)

This project caps the `cctv-clips/events/` frame buffer **in Supabase**, not on the
ESP32. The device just uploads uniquely-named, timestamped frames
(`events/frame_<UTC>_<seq>.jpg`, see issue #11) and a scheduled Postgres job
deletes the oldest objects beyond a fixed count.

**Why server-side?**
- No extra requests from the ESP32 (no per-upload DELETE, no NVS ring bookkeeping).
- Retention logic lives in Supabase where it is easy to change, not in fragile
  firmware.
- The firmware needs **no changes** — the #11 fix already produces sortable,
  timestamped names, which is exactly what `order by created_at` needs.

> Supabase has **no built-in "keep newest N / evict oldest"** bucket setting.
> This is implemented with the `pg_cron` extension + a small SQL job.

---

## What "FIFO up to a size limit" means here
- Keep the **newest N** objects in `events/` (N = your `STORAGE_FRAME_LIMIT`, default **200**).
- When there are more than N, delete the **oldest** ones (ordered by `created_at`).
- The device keeps uploading as-is; the sweep runs on a schedule (e.g. every minute).
- The count is bounded **eventually** (each sweep), not exactly at every instant —
  between sweeps it may briefly exceed N. For a CCTV frame buffer that is fine; if
  you need exact real-time capping, use the trigger variant at the bottom.

---

## Step 1 — Enable the `pg_cron` extension
Supabase Dashboard → **Database → Extensions** → search `pg_cron` → **Enable**.
(Equivalently: `create extension if not exists pg_cron;` in the SQL editor.)

## Step 2 — Schedule the FIFO eviction job
Supabase Dashboard → **SQL Editor** → run:

```sql
-- Keep the newest 200 frames in cctv-clips/events/, delete the rest.
-- Change 200 to match STORAGE_FRAME_LIMIT in your firmware config.h.
-- Change the cron expression '* * * * *' (every minute) to taste.
select cron.schedule(
  'cctv-fifo-evict',        -- job name (unique)
  '* * * * *',              -- every minute
  $$
  delete from storage.objects
  where bucket_id = 'cctv-clips'
    and name like 'events/%'
    and id in (
      select id from storage.objects
      where bucket_id = 'cctv-clips'
        and name like 'events/%'
      order by created_at desc
      offset 200            -- = STORAGE_FRAME_LIMIT (keep newest 200)
    );
  $$
);
```

- Deleting the row in `storage.objects` deletes the underlying file.
- `offset 200` keeps the newest 200 and removes everything older.
- The job name `cctv-fifo-evict` must be unique; re-running `cron.schedule` with the
  same name updates it.

## Step 3 — Verify
Check the schedule and recent runs:

```sql
-- Is the job registered?
select jobid, schedule, jobname, active from cron.job where jobname = 'cctv-fifo-evict';

-- Did it run? (most recent executions)
select jobid, status, return_message, start_time, end_time
from cron.job_run_details
order by start_time desc
limit 10;

-- How many frames are currently retained?
select count(*) from storage.objects
where bucket_id = 'cctv-clips' and name like 'events/%';
```

`count(*)` should settle at or near your limit after the buffer fills and the job runs.

## Changing the limit or interval
- **Different size:** change both `offset N` (Step 2) and, for consistency, keep it in
  sync with `STORAGE_FRAME_LIMIT` in `config.h` (the firmware value is informational
  for this server-side approach, but keeping them equal avoids confusion).
- **Different cadence:** edit the cron expression, e.g. `'*/5 * * * *'` (every 5 min).
- **Remove the job:** `select cron.unschedule('cctv-fifo-evict');`

---

## Alternative: exact real-time capping (trigger variant)
If you want the count capped on **every** insert instead of on a schedule, use an
`AFTER INSERT` trigger on `storage.objects` (fires per upload — more work than the
cron sweep, but never exceeds N):

```sql
create or replace function public.cctv_evict_old_frames()
returns trigger language plpgsql security definer as $$
begin
  if new.bucket_id = 'cctv-clips' and new.name like 'events/%' then
    delete from storage.objects
    where bucket_id = 'cctv-clips'
      and name like 'events/%'
      and id in (
        select id from storage.objects
        where bucket_id = 'cctv-clips' and name like 'events/%'
        order by created_at desc
        offset 200            -- = STORAGE_FRAME_LIMIT
      );
  end if;
  return new;
end;
$$;

drop trigger if exists cctv_evict_after_insert on storage.objects;
create trigger cctv_evict_after_insert
after insert on storage.objects
for each row execute function public.cctv_evict_old_frames();
```

Use **either** the cron job **or** the trigger, not both. The `pg_cron` sweep (Steps
1–2) is recommended for a low-traffic CCTV buffer.

---

## Notes
- **Time-based instead of count-based:** if you prefer "delete anything older than
  24h" rather than "keep newest N", replace the delete predicate with
  `and created_at < now() - interval '24 hours'`. That bounds by *age*, not object
  count — bursts can still spike the count.
- **RLS:** the eviction runs as a scheduled/`security definer` server job, so it is not
  subject to the anon RLS policies your device uploads use.
