export type QlicBytes = ArrayBuffer | ArrayBufferView;

export interface QlicFrame {
  width: number;
  height: number;
  delay: number;
  rgba: Uint8ClampedArray;
}

export interface QlicImage {
  width: number;
  height: number;
  frames: QlicFrame[];
  loopCount: number;
  animated: boolean;
}

export interface QlicWideImage {
  width: number;
  height: number;
  channels: 1 | 3 | 4;
  bitsPerSample: number;
  stride: number;
  samples: Uint16Array | Uint32Array;
}

export interface QlicCicp {
  colorPrimaries: number;
  transferCharacteristics: number;
  matrixCoefficients: number;
  fullRange: boolean;
}

export interface QlicMasteringDisplay {
  primaries: Array<{ x: number; y: number }>;
  whitePoint: { x: number; y: number };
  maxLuminance: number;
  minLuminance: number;
}

export interface QlicContentLight {
  maxContentLightLevel: number;
  maxFrameAverageLightLevel: number;
}

export interface QlicMetadataBlock {
  tag: string;
  data: Uint8Array;
}

export interface QlicHdrImage {
  pixels: QlicWideImage;
  sampleType: "uint";
  alphaAssociation: "none" | "straight" | "premultiplied";
  colorAuthority: "unspecified" | "icc" | "cicp" | "icc-preferred" | "cicp-preferred";
  icc: Uint8Array | null;
  cicp: QlicCicp | null;
  masteringDisplay: QlicMasteringDisplay | null;
  contentLight: QlicContentLight | null;
  metadata: QlicMetadataBlock[];
}

export interface CreateQlicOptions {
  wasmUrl?: string | URL;
  wasmBinary?: BufferSource;
}

export interface QlicApi {
  encode(input: QlicBytes, width: number, height: number): Uint8Array;
  decode(input: QlicBytes): QlicImage;
  validate(input: QlicBytes): true;
  validateUrl(url: RequestInfo | URL, fetchOptions?: RequestInit): Promise<true>;
  validateBlob(blob: Blob): Promise<true>;
  decodeUrl(url: RequestInfo | URL, fetchOptions?: RequestInit): Promise<QlicImage>;
  decodeBlob(blob: Blob): Promise<QlicImage>;
  decodeWide(input: QlicBytes): QlicWideImage;
  decodeWideUrl(url: RequestInfo | URL, fetchOptions?: RequestInit): Promise<QlicWideImage>;
  decodeWideBlob(blob: Blob): Promise<QlicWideImage>;
  decodeHdr(input: QlicBytes): QlicHdrImage;
  decodeHdrUrl(url: RequestInfo | URL, fetchOptions?: RequestInit): Promise<QlicHdrImage>;
  decodeHdrBlob(blob: Blob): Promise<QlicHdrImage>;
  encodePng(input: QlicBytes, width: number, height: number): Promise<Uint8Array>;
  firstImageData(input: QlicImage | QlicBytes): ImageData;
  imageData(frame: QlicFrame): ImageData;
  draw(
    input: QlicImage | QlicBytes,
    canvas: HTMLCanvasElement,
    frameIndex?: number,
  ): QlicImage;
  play(input: QlicImage | QlicBytes, canvas: HTMLCanvasElement): () => void;
}

export function createQlic(options?: CreateQlicOptions): Promise<QlicApi>;
export default createQlic;
