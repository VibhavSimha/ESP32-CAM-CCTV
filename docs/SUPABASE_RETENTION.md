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
> This is implemented with the `pg_cron` + `pg_net` extensions + a small SQL job.

---

## What "FIFO up to a size limit" means here
- Keep the **newest N** objects in `events/` (N = your `STORAGE_FRAME_LIMIT`, default **200**).
- When there are more than N, delete the **oldest** ones (ordered by `created_at`).
- The device keeps uploading as-is; the sweep runs on a schedule (e.g. every minute).
- The count is bounded **eventually** (each sweep), not exactly at every instant —
  between sweeps it may briefly exceed N. For a CCTV frame buffer that is fine; if
  you need exact real-time capping, use the trigger variant at the bottom.

---

## Step 1 — Enable the `pg_cron` and `pg_net` extensions
The sweep is scheduled with **`pg_cron`** and the actual file deletion is done by
calling the **Storage REST API** over HTTP with **`pg_net`** (see the box below for
why a plain SQL `DELETE` cannot work).

Supabase Dashboard → **Database → Extensions**, then enable both:
- `pg_cron` (scheduling)
- `pg_net`  (async HTTP from Postgres)

(Equivalently in the SQL editor:
`create extension if not exists pg_cron;` and
`create extension if not exists pg_net;`)

> **Why HTTP and not `DELETE FROM storage.objects`?**
> `storage.objects` is only the *metadata* table. Deleting a row there leaves the
> real file orphaned in the storage backend, so Supabase blocks it with the
> `storage.protect_delete()` trigger (see [Troubleshooting](#troubleshooting)).
> There is **no** `storage.delete_object()` SQL function — the only supported way
> to remove a file from SQL is to call the Storage API, which `pg_net` lets us do.

## Step 1b — Store your `service_role` key in Vault
Deleting objects requires the **`service_role`** key (it bypasses RLS). Never paste
it in plain text into the job; store it once in Supabase Vault instead
(**Project Settings → API** has the key; **SQL Editor** to store it):

```sql
-- Store the service_role key so the cron job can read it securely.
select vault.create_secret(
  'YOUR_SERVICE_ROLE_KEY',   -- paste the service_role key here (kept encrypted)
  'service_role_key'         -- secret name referenced by the job below
);
```

## Step 2 — Schedule the FIFO eviction job
Supabase Dashboard → **SQL Editor** → run (replace `YOUR_PROJECT_REF` with your
project ref, e.g. `abcd1234` from `https://abcd1234.supabase.co`):

```sql
-- Keep the newest 200 frames in cctv-clips/events/, delete the rest.
-- Change 200 to match STORAGE_FRAME_LIMIT in your firmware config.h.
-- Change the cron expression '* * * * *' (every minute) to taste.
select cron.schedule(
  'cctv-fifo-evict',        -- job name (unique)
  '* * * * *',              -- every minute
  $$
  -- For each object beyond the newest 200, issue an async HTTP DELETE to the
  -- Storage API. This removes BOTH the file and its storage.objects row.
  select net.http_delete(
    url := 'https://YOUR_PROJECT_REF.supabase.co/storage/v1/object/cctv-clips/'
           || name,
    headers := jsonb_build_object(
      'Authorization',
      'Bearer ' || (select decrypted_secret from vault.decrypted_secrets
                    where name = 'service_role_key'),
      'apikey',
      (select decrypted_secret from vault.decrypted_secrets
       where name = 'service_role_key')
    )
  )
  from storage.objects
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

- `net.http_delete(...)` calls the Storage API path
  `/storage/v1/object/<bucket>/<name>`, which deletes the file **and** its metadata
  row atomically — the supported alternative to a direct SQL delete.
- `offset 200` keeps the newest 200 and removes everything older.
- The `service_role` key is read from Vault at run time, so it is never stored in
  the job definition in plain text.
- The frame names (`events/frame_<UTC>_<seq>.jpg`, issue #11) are URL-safe, so no
  extra encoding is needed. If you change the naming scheme to include characters
  like spaces or `#`, wrap `name` accordingly.
- The job name `cctv-fifo-evict` must be unique; re-running `cron.schedule` with the
  same name updates it.

> **Note:** `YOUR_PROJECT_REF` appears here **and** in the trigger variant below. If
> you use both or switch between them, update every occurrence to the same value.

## Step 3 — Verify it works
Run these checks a minute or two after scheduling (let the job fire at least once).

```sql
-- 1. Is the job registered and active?
select jobid, schedule, jobname, active from cron.job where jobname = 'cctv-fifo-evict';

-- 2. Did the cron job run without SQL errors?
--    status should be 'succeeded'. A failure here means the SQL itself failed.
select jobid, status, return_message, start_time, end_time
from cron.job_run_details
order by start_time desc
limit 10;

-- 3. Did the Storage API actually accept the DELETEs?
--    pg_net records each HTTP call's result here. status_code 200 = deleted OK.
--    (401/403 => wrong/missing service_role key; 404 => object already gone.)
select id, status_code, content, created
from net._http_response
order by created desc
limit 10;

-- 4. How many frames are currently retained?
select count(*) from storage.objects
where bucket_id = 'cctv-clips' and name like 'events/%';
```

**What "working" looks like:**
- Check 1 shows the job `active = true`.
- Check 2 shows `status = 'succeeded'` (no `protect_delete` or other error).
- Check 3 shows `status_code = 200` for the delete calls.
- Check 4 `count(*)` settles at or near your limit (200) once the buffer fills.

If check 3 shows `401`/`403`, the `service_role_key` secret is missing or wrong —
re-run the `vault.create_secret` step. If check 2 shows an error mentioning
`net` / `http_delete`, the `pg_net` extension is not enabled (Step 1).

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
declare
  svc_key text;
begin
  if new.bucket_id = 'cctv-clips' and new.name like 'events/%' then
    -- Read the service_role key from Vault once per trigger execution.
    select decrypted_secret into svc_key
    from vault.decrypted_secrets where name = 'service_role_key';
    -- Delete each object beyond the newest 200 via the Storage API (pg_net).
    perform net.http_delete(
      url := 'https://YOUR_PROJECT_REF.supabase.co/storage/v1/object/cctv-clips/'
             || obj.name,
      headers := jsonb_build_object(
        'Authorization', 'Bearer ' || svc_key,
        'apikey', svc_key
      )
    )
    from (
      select name from storage.objects
      where bucket_id = 'cctv-clips' and name like 'events/%'
      order by created_at desc
      offset 200            -- = STORAGE_FRAME_LIMIT
    ) obj;
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

---

## Troubleshooting

### `storage.protect_delete()` — "Direct deletion from storage tables is not allowed"

```
ERROR: Direct deletion from storage tables is not allowed. Use the Storage API instead.
HINT: This prevents accidental data loss from orphaned objects.
CONTEXT: PL/pgSQL function storage.protect_delete() line 5 at RAISE
```

**What it means.** Supabase added a `BEFORE DELETE` trigger called `storage.protect_delete()`
to the `storage.objects` table. A plain SQL `DELETE FROM storage.objects` only removes
the metadata row from the database; the actual file in object storage (S3 / the Supabase
storage backend) is **not** deleted, leaving an orphaned file that wastes space and
can never be cleaned up. The trigger blocks the direct delete and forces you to use
the Storage API, which deletes both the row and the physical file atomically.

**How to spot it.** Run the verification query from Step 3:

```sql
select jobid, status, return_message, start_time, end_time
from cron.job_run_details
order by start_time desc
limit 10;
```

If `status` is `failed` and `return_message` contains `protect_delete`, the cron job
is using a direct `DELETE FROM storage.objects` (or an assumed `storage.delete_object()`
helper — **which does not exist** in Supabase).

**How to fix it.** You cannot delete storage files from raw SQL at all. Use the
`pg_net` + Storage API approach shown in Steps 1–2: schedule the job so it calls
`net.http_delete(...)` against `/storage/v1/object/<bucket>/<name>` with your
`service_role` key. That endpoint removes the file and its metadata row together.
If you set up the job before this document was updated, drop and recreate it:

```sql
-- Remove the old (broken) job and recreate it with the pg_net version:
select cron.unschedule('cctv-fifo-evict');
-- Then re-run the cron.schedule(...) block from Step 2 above.
```

> The same rule applies to the Storage Dashboard/API: `.remove([...])` in a Supabase
> client library, or a `DELETE /storage/v1/object/...` REST call, are the supported
> ways to delete — never a SQL `DELETE` on `storage.objects`.
