-- Track how many times a deployment has failed to build/start so the orchestrator
-- can stop relaunching a hopeless deployment instead of looping forever.
ALTER TABLE deployments ADD COLUMN deploy_attempts INTEGER NOT NULL DEFAULT 0;
