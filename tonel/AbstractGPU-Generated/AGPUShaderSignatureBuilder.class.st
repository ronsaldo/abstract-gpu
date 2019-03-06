Class {
	#name : #AGPUShaderSignatureBuilder,
	#superclass : #AGPUInterface,
	#category : 'AbstractGPU-Generated'
}

{ #category : #'wrappers' }
AGPUShaderSignatureBuilder >> addReference [
	| resultValue_ |
	resultValue_ := AGPUCBindings uniqueInstance addReference_shader_signature_builder: (self validHandle).
	self checkErrorCode: resultValue_
]

{ #category : #'wrappers' }
AGPUShaderSignatureBuilder >> primitiveRelease [
	| resultValue_ |
	resultValue_ := AGPUCBindings uniqueInstance release_shader_signature_builder: (self validHandle).
	self checkErrorCode: resultValue_
]

{ #category : #'wrappers' }
AGPUShaderSignatureBuilder >> build [
	| resultValue_ |
	resultValue_ := AGPUCBindings uniqueInstance build_shader_signature_builder: (self validHandle).
	^ AGPUShaderSignature forHandle: resultValue_
]

{ #category : #'wrappers' }
AGPUShaderSignatureBuilder >> addBindingConstant [
	| resultValue_ |
	resultValue_ := AGPUCBindings uniqueInstance addBindingConstant_shader_signature_builder: (self validHandle).
	self checkErrorCode: resultValue_
]

{ #category : #'wrappers' }
AGPUShaderSignatureBuilder >> addBindingElement: type maxBindings: maxBindings [
	| resultValue_ |
	resultValue_ := AGPUCBindings uniqueInstance addBindingElement_shader_signature_builder: (self validHandle) type: type maxBindings: maxBindings.
	self checkErrorCode: resultValue_
]

{ #category : #'wrappers' }
AGPUShaderSignatureBuilder >> beginBindingBank: maxBindings [
	| resultValue_ |
	resultValue_ := AGPUCBindings uniqueInstance beginBindingBank_shader_signature_builder: (self validHandle) maxBindings: maxBindings.
	self checkErrorCode: resultValue_
]

{ #category : #'wrappers' }
AGPUShaderSignatureBuilder >> addBindingBankElement: type bindingPointCount: bindingPointCount [
	| resultValue_ |
	resultValue_ := AGPUCBindings uniqueInstance addBindingBankElement_shader_signature_builder: (self validHandle) type: type bindingPointCount: bindingPointCount.
	self checkErrorCode: resultValue_
]
