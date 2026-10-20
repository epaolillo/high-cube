import { Button, Modal, Slider, Tabs, Typography, Upload, message } from 'antd';
import type { UploadProps } from 'antd';
import { useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { AppImageEntry, AppImagePicker } from './AppImagePicker';

// Small, square visual used for an application. Falls back to the first letter
// of the name when no icon is set yet.
export function AppIcon({
  icon,
  name,
  size = 28,
}: {
  icon?: string;
  name?: string;
  size?: number;
}) {
  if (icon) {
    return (
      <img
        src={icon}
        alt=""
        width={size}
        height={size}
        style={{ width: size, height: size, objectFit: 'cover', borderRadius: 6, flex: `0 0 ${size}px` }}
      />
    );
  }
  return (
    <span
      style={{
        width: size,
        height: size,
        borderRadius: 6,
        background: '#E3E8EE',
        color: '#425466',
        display: 'inline-flex',
        alignItems: 'center',
        justifyContent: 'center',
        fontSize: Math.round(size * 0.45),
        fontWeight: 600,
        flex: `0 0 ${size}px`,
        textTransform: 'uppercase',
      }}
    >
      {(name || '?').slice(0, 1)}
    </span>
  );
}

const VIEWPORT = 256; // on-screen crop square
const OUTPUT = 128; // exported icon size (square)

// Canvas cropper that keeps the image covering a square viewport. The user can
// zoom and drag; the visible square is exported as a downscaled PNG data URL.
function SquareCropper({ src, onChange }: { src: string; onChange: (dataUrl: string) => void }) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const imgRef = useRef<HTMLImageElement | null>(null);
  const baseRef = useRef(1); // "cover" scale at zoom = 1
  const offsetRef = useRef({ x: 0, y: 0 });
  const dragRef = useRef<{ x: number; y: number; ox: number; oy: number } | null>(null);
  const [scale, setScale] = useState(1);
  const [ready, setReady] = useState(false);

  const clamp = () => {
    const img = imgRef.current;
    if (!img) return;
    const w = img.naturalWidth * baseRef.current * scale;
    const h = img.naturalHeight * baseRef.current * scale;
    offsetRef.current.x = Math.min(0, Math.max(VIEWPORT - w, offsetRef.current.x));
    offsetRef.current.y = Math.min(0, Math.max(VIEWPORT - h, offsetRef.current.y));
  };

  const redraw = () => {
    const canvas = canvasRef.current;
    const img = imgRef.current;
    if (!canvas || !img) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    ctx.clearRect(0, 0, VIEWPORT, VIEWPORT);
    const w = img.naturalWidth * baseRef.current * scale;
    const h = img.naturalHeight * baseRef.current * scale;
    ctx.drawImage(img, offsetRef.current.x, offsetRef.current.y, w, h);
  };

  const emit = () => {
    const img = imgRef.current;
    if (!img) return;
    const out = document.createElement('canvas');
    out.width = OUTPUT;
    out.height = OUTPUT;
    const ctx = out.getContext('2d');
    if (!ctx) return;
    const r = OUTPUT / VIEWPORT;
    const w = img.naturalWidth * baseRef.current * scale * r;
    const h = img.naturalHeight * baseRef.current * scale * r;
    ctx.drawImage(img, offsetRef.current.x * r, offsetRef.current.y * r, w, h);
    onChange(out.toDataURL('image/png'));
  };

  // Load (or reload) the source image and center it.
  useEffect(() => {
    const img = new Image();
    img.onload = () => {
      imgRef.current = img;
      baseRef.current = VIEWPORT / Math.min(img.naturalWidth, img.naturalHeight);
      const w = img.naturalWidth * baseRef.current;
      const h = img.naturalHeight * baseRef.current;
      offsetRef.current = { x: (VIEWPORT - w) / 2, y: (VIEWPORT - h) / 2 };
      setScale(1);
      setReady(true);
      redraw();
      emit();
    };
    img.src = src;
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [src]);

  // Re-render and re-export whenever the zoom changes.
  useEffect(() => {
    if (!ready) return;
    clamp();
    redraw();
    emit();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [scale, ready]);

  const onPointerDown = (e: React.MouseEvent<HTMLCanvasElement>) => {
    dragRef.current = {
      x: e.clientX,
      y: e.clientY,
      ox: offsetRef.current.x,
      oy: offsetRef.current.y,
    };
  };
  const onPointerMove = (e: React.MouseEvent<HTMLCanvasElement>) => {
    const drag = dragRef.current;
    if (!drag) return;
    offsetRef.current.x = drag.ox + (e.clientX - drag.x);
    offsetRef.current.y = drag.oy + (e.clientY - drag.y);
    clamp();
    redraw();
  };
  const endDrag = () => {
    if (!dragRef.current) return;
    dragRef.current = null;
    emit();
  };

  return (
    <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 12 }}>
      <canvas
        ref={canvasRef}
        width={VIEWPORT}
        height={VIEWPORT}
        onMouseDown={onPointerDown}
        onMouseMove={onPointerMove}
        onMouseUp={endDrag}
        onMouseLeave={endDrag}
        style={{
          width: VIEWPORT,
          height: VIEWPORT,
          borderRadius: 12,
          border: '1px solid #E3E8EE',
          background:
            'repeating-conic-gradient(#f3f4f6 0% 25%, #fff 0% 50%) 50% / 20px 20px',
          cursor: 'move',
          touchAction: 'none',
        }}
      />
      <div style={{ width: VIEWPORT }}>
        <Slider min={1} max={4} step={0.01} value={scale} onChange={setScale} tooltip={{ open: false }} />
      </div>
    </div>
  );
}

export function IconEditor({
  open,
  value,
  name,
  onCancel,
  onSave,
}: {
  open: boolean;
  value?: string;
  name?: string;
  onCancel: () => void;
  onSave: (icon: string) => void | Promise<void>;
}) {
  const { t } = useTranslation();
  const [draft, setDraft] = useState<string>(value || '');
  const [uploadSrc, setUploadSrc] = useState<string>();
  const [saving, setSaving] = useState(false);

  useEffect(() => {
    if (open) {
      setDraft(value || '');
      setUploadSrc(undefined);
    }
  }, [open, value]);

  const beforeUpload: UploadProps['beforeUpload'] = (file) => {
    if (!file.type.startsWith('image/')) {
      message.error(t('iconUploadInvalid'));
      return Upload.LIST_IGNORE;
    }
    const reader = new FileReader();
    reader.onload = () => setUploadSrc(String(reader.result));
    reader.readAsDataURL(file);
    return false; // handle locally, never upload to the server
  };

  const save = async () => {
    setSaving(true);
    try {
      await onSave(draft);
    } finally {
      setSaving(false);
    }
  };

  return (
    <Modal
      open={open}
      title={t('iconEditTitle')}
      onCancel={onCancel}
      width={640}
      footer={[
        <Button key="remove" danger onClick={() => setDraft('')} disabled={!draft}>
          {t('iconRemove')}
        </Button>,
        <Button key="cancel" onClick={onCancel}>
          {t('cancel')}
        </Button>,
        <Button key="save" type="primary" loading={saving} onClick={save}>
          {t('save')}
        </Button>,
      ]}
    >
      <div style={{ display: 'flex', alignItems: 'center', gap: 12, marginBottom: 12 }}>
        <Typography.Text type="secondary">{t('iconPreview')}:</Typography.Text>
        <AppIcon icon={draft} name={name} size={48} />
      </div>
      <Tabs
        items={[
          {
            key: 'catalog',
            label: t('iconFromCatalog'),
            children: (
              <AppImagePicker onSelect={(entry: AppImageEntry) => setDraft(`/app-images/${entry.logo}`)} />
            ),
          },
          {
            key: 'upload',
            label: t('iconUpload'),
            children: (
              <div>
                {uploadSrc ? (
                  <div>
                    <SquareCropper src={uploadSrc} onChange={setDraft} />
                    <div style={{ textAlign: 'center', marginTop: 8 }}>
                      <Button size="small" onClick={() => setUploadSrc(undefined)}>
                        {t('iconChooseAnother')}
                      </Button>
                    </div>
                  </div>
                ) : (
                  <Upload.Dragger
                    accept="image/*"
                    multiple={false}
                    showUploadList={false}
                    beforeUpload={beforeUpload}
                  >
                    <p style={{ fontSize: 32, margin: 0, color: '#635bff' }}>+</p>
                    <p>{t('iconUploadHint')}</p>
                  </Upload.Dragger>
                )}
              </div>
            ),
          },
        ]}
      />
    </Modal>
  );
}
