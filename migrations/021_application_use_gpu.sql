-- Optional NVIDIA GPU passthrough for an application's containers.
-- When enabled the agent runs the container with `docker run --gpus all`, so
-- the host GPU is visible inside (requires the nvidia-container-toolkit on the
-- server). Defaults to off so existing applications keep running on CPU.
ALTER TABLE applications ADD COLUMN use_gpu INTEGER NOT NULL DEFAULT 0;
